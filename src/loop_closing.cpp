#include <std_msgs/Int32.h>

#include <numeric>

#include "loop_closing.h"
#include "tools.h"

using namespace tools;

namespace slam
{

  LoopClosing::LoopClosing()
  {
    ros::NodeHandle nhp("~");
    pub_num_lc_ = nhp.advertise<std_msgs::Int32>("loop_closings", 2, true);
    pub_queue_ = nhp.advertise<std_msgs::Int32>("loop_closing_queue", 2, true);
  }

  void LoopClosing::run()
  {
    // Init
    execution_dir_ = WORKING_DIRECTORY + "loop_closing";
    if (fs::is_directory(execution_dir_))
      fs::remove_all(execution_dir_);
    fs::path dir(execution_dir_);
    if (!fs::create_directory(dir))
      ROS_ERROR("[Localization:] ERROR -> Impossible to create the loop_closing directory.");

    // Loop
    ros::Rate r(500);
    while(ros::ok())
    {
      if(checkNewClusterInQueue())
      {
        processNewCluster();

        searchInNeigborhood();

        searchByHash();
      }

      if (pub_num_lc_.getNumSubscribers() > 0)
      {
        std_msgs::Int32 msg;
        msg.data = lc_found_.size();
        pub_num_lc_.publish(msg);
      }

      if (pub_queue_.getNumSubscribers() > 0)
      {
        mutex::scoped_lock lock(mutex_cluster_queue_);
        std_msgs::Int32 msg;
        msg.data = cluster_queue_.size();
        pub_queue_.publish(msg);
      }

      r.sleep();
    }
  }

  void LoopClosing::addClusterToQueue(Cluster cluster)
  {
    mutex::scoped_lock lock(mutex_cluster_queue_);
    cluster_queue_.push_back(cluster);
  }

  bool LoopClosing::checkNewClusterInQueue()
  {
    mutex::scoped_lock lock(mutex_cluster_queue_);
    return(!cluster_queue_.empty());
  }

  void LoopClosing::processNewCluster()
  {
    // Get the cluster
    {
      mutex::scoped_lock lock(mutex_cluster_queue_);
      c_cluster_ = cluster_queue_.front();
      cluster_queue_.pop_front();
    }

    // Initialize hash
    if (!hash_.isInitialized())
      hash_.init(c_cluster_.getSift());

    // Save hash to table
    hash_table_.push_back(make_pair(c_cluster_.getId(), hash_.getHash(c_cluster_.getSift())));

    // Store
    cv::FileStorage fs(execution_dir_+"/"+lexical_cast<string>(c_cluster_.getId())+".yml", cv::FileStorage::WRITE);
    write(fs, "frame_id", c_cluster_.getFrameId());
    write(fs, "pose", Tools::transformToMat(c_cluster_.getPose()) );
    write(fs, "kp", c_cluster_.getKp());
    write(fs, "desc", c_cluster_.getOrb());
    write(fs, "threed", c_cluster_.getPoints());
    fs.release();
  }

  void LoopClosing::searchInNeigborhood()
  {
    // Init
    int id = c_cluster_.getId();
    int current_frame_id = c_cluster_.getFrameId();
    int processed_neighbors = 0;
    int neighbor_id = id - 1;
    vector<cv::KeyPoint> kp = c_cluster_.getKp();

    // Store matchings
    vector<cv::Point2f> matched_kp;
    vector<cv::Point3f> matched_points;

    // Loop over neighbors of different frames
    while (processed_neighbors < LC_NEIGHBORS && neighbor_id >= 0)
    {
      // Extract the neighbor
      Cluster neighbor = readCluster(neighbor_id);
      vector<cv::Point3f> neighbor_points = neighbor.getPoints();


      if (neighbor.getFrameId() != current_frame_id)
      {
        vector<cv::DMatch> matches;
        Tools::ratioMatching(c_cluster_.getOrb(), neighbor.getOrb(), 0.8, matches);

        // Compute the percentage of matchings
        int size_c = c_cluster_.getOrb().rows;
        int size_n = neighbor.getOrb().rows;
        int m_percentage = round(100.0 * (float) matches.size() / (float) min(size_c, size_n) );

        // Only consider clusters with high number of matchings
        if (m_percentage > 50)
        {
          for(uint i=0; i<matches.size(); i++)
          {
            matched_kp.push_back(kp[matches[i].queryIdx].pt);

            // Transform camera point to world coordinates
            cv::Point3f p = neighbor_points[matches[i].trainIdx];
            tf::Vector3 p_tf(p.x, p.y, p.z);
            tf::Vector3 p_tf_world = neighbor.getPose() * p_tf;
            cv::Point3f p_world(p_tf_world.x(), p_tf_world.y(), p_tf_world.z());
            matched_points.push_back(p_world);
          }
        }
        processed_neighbors++;
      }
      neighbor_id--;
    }

    // Estimate the motion
    vector<int> inliers;
    cv::Mat rvec, tvec;
    solvePnPRansac(matched_points, matched_kp, graph_->getCameraMatrix(),
                   cv::Mat(), rvec, tvec, false,
                   100, 1.3, MAX_INLIERS, inliers);

    ROS_INFO_STREAM("KKK: " << processed_neighbors << " | " << matched_points.size() << ", " << inliers.size());

  }


  void LoopClosing::searchByHash()
  {
    // Get the candidates to close loop
    vector< pair<int,float> > hash_matching;
    getCandidates(c_cluster_.getId(), hash_matching);
    if (hash_matching.size() == 0) return;


  }

  void LoopClosing::getCandidates(int cluster_id, vector< pair<int,float> >& candidates)
  {
    // Init
    candidates.clear();

    // Check if enough neighbors
    if ((int)hash_table_.size() <= LC_NEIGHBORS) return;

    // Create a list with the non-possible candidates (because they are already loop closings)
    vector<int> no_candidates;
    for (uint i=0; i<lc_found_.size(); i++)
    {
      if (lc_found_[i].first == cluster_id)
        no_candidates.push_back(lc_found_[i].second);
      if (lc_found_[i].second == cluster_id)
        no_candidates.push_back(lc_found_[i].first);
    }

    // Query hash
    vector<float> hash_q = hash_table_[cluster_id].second;

    // Loop over all the hashes stored
    vector< pair<int,float> > all_matchings;
    for (uint i=0; i<hash_table_.size()-LC_NEIGHBORS-1; i++)
    {
      if (i > hash_table_.size()-1 ) continue;

      // Do not compute the hash matching with itself
      if (hash_table_[i].first == cluster_id) continue;

      // Continue if candidate is in the no_candidates list
      if (find(no_candidates.begin(), no_candidates.end(), hash_table_[i].first) != no_candidates.end())
        continue;

      // Hash matching
      vector<float> hash_t = hash_table_[i].second;
      float m = hash_.match(hash_q, hash_t);
      all_matchings.push_back(make_pair(hash_table_[i].first, m));
    }

    // Sort the hash matchings
    sort(all_matchings.begin(), all_matchings.end(), Tools::sortByMatching);

    // Retrieve the best n matches
    uint max_size = 5;
    if (max_size > all_matchings.size()) max_size = all_matchings.size();
    for (uint i=0; i<max_size; i++)
      candidates.push_back(all_matchings[i]);
  }

  Cluster LoopClosing::readCluster(int id)
  {
    string file = execution_dir_+"/"+lexical_cast<string>(id)+".yml";
    Cluster cluster;

    // Sanity check
    if ( !fs::exists(file) ) return cluster;

    cv::FileStorage fs;
    fs.open(file, cv::FileStorage::READ);
    if (!fs.isOpened()) return cluster;

    int frame_id;
    vector<cv::KeyPoint> kp;
    cv::Mat desc, pose, empty;
    vector<cv::Point3f> points;
    fs["frame_id"] >> frame_id;
    fs["pose"] >> pose;
    fs["desc"] >> desc;
    fs["threed"] >> points;
    cv::FileNode kp_node = fs["kp"];
    read(kp_node, kp);
    fs.release();

    // Set the properties of the cluster
    Cluster cluster_tmp(id, frame_id, Tools::matToTransform(pose), kp, desc, empty, points);
    cluster = cluster_tmp;

    return cluster;
  }

  void LoopClosing::finalize()
  {
    // Remove the temporal directory
    if (fs::is_directory(execution_dir_))
      fs::remove_all(execution_dir_);
  }

} //namespace slam