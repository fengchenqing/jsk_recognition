// -*- mode: c++ -*-
/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014, JSK Lab
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/o2r other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/
#define BOOST_PARAMETER_MAX_ARITY 7
#include "jsk_pcl_ros/plane_concatenator.h"
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include "jsk_pcl_ros/pcl_conversion_util.h"
#include "jsk_pcl_ros/pcl_util.h"

namespace jsk_pcl_ros
{
  void PlaneConcatenator::onInit()
  {
    DiagnosticNodelet::onInit();

    srv_ = boost::make_shared <dynamic_reconfigure::Server<Config> > (*pnh_);
    dynamic_reconfigure::Server<Config>::CallbackType f =
      boost::bind (
        &PlaneConcatenator::configCallback, this, _1, _2);
    srv_->setCallback (f);
    
    pub_indices_ = advertise<ClusterPointIndices>(
      *pnh_, "output/indices", 1);
    pub_polygon_ = advertise<PolygonArray>(
      *pnh_, "output/polygons", 1);
    pub_coefficients_ = advertise<ModelCoefficientsArray>(
      *pnh_, "output/coefficients", 1);
  }

  void PlaneConcatenator::subscribe()
  {
    sub_cloud_.subscribe(*pnh_, "input", 1);
    sub_indices_.subscribe(*pnh_, "input/indices", 1);
    sub_polygon_.subscribe(*pnh_, "input/polygons", 1);
    sub_coefficients_.subscribe(*pnh_, "input/coefficients", 1);
    sync_ = boost::make_shared<message_filters::Synchronizer<SyncPolicy> > (100);
    sync_->connectInput(sub_cloud_, sub_indices_,
                        sub_polygon_, sub_coefficients_);
    sync_->registerCallback(boost::bind(&PlaneConcatenator::concatenate, this,
                                        _1, _2, _3, _4));
  }

  void PlaneConcatenator::unsubscribe()
  {
    sub_cloud_.unsubscribe();
    sub_indices_.unsubscribe();
    sub_polygon_.unsubscribe();
    sub_coefficients_.unsubscribe();
  }

  void PlaneConcatenator::concatenate(
    const sensor_msgs::PointCloud2::ConstPtr& cloud_msg,
    const ClusterPointIndices::ConstPtr& indices_msg,
    const PolygonArray::ConstPtr& polygon_array_msg,
    const ModelCoefficientsArray::ConstPtr& coefficients_array_msg)
  {
    boost::mutex::scoped_lock lock(mutex_);
    vital_checker_->poke();
    
    size_t nr_cluster = polygon_array_msg->polygons.size();
    pcl::PointCloud<PointT>::Ptr cloud (new pcl::PointCloud<PointT>);
    pcl::fromROSMsg(*cloud_msg, *cloud);
    // first convert to polygon instance
    std::vector<pcl::ModelCoefficients::Ptr> all_coefficients
      = pcl_conversions::convertToPCLModelCoefficients(
        coefficients_array_msg->coefficients);
    std::vector<pcl::PointIndices::Ptr> all_indices
      = pcl_conversions::convertToPCLPointIndices(indices_msg->cluster_indices);
    std::vector<pcl::PointCloud<PointT>::Ptr> all_clouds
      = convertToPointCloudArray<PointT>(cloud, all_indices);
    std::vector<Plane::Ptr> planes = convertToPlanes(all_coefficients);
    // build connection map
    IntegerGraphMap connection_map;
    for (size_t i = 0; i < nr_cluster; i++) {
      connection_map[i] = std::vector<int>();
      connection_map[i].push_back(i);
      // build kdtree
      pcl::KdTreeFLANN<PointT> kdtree;
      pcl::PointCloud<PointT>::Ptr focused_cloud = all_clouds[i];
      kdtree.setInputCloud(focused_cloud);
      // look up near polygon
      Plane::Ptr focused_plane = planes[i];
      for (size_t j = i + 1; j < nr_cluster; j++) {
        Plane::Ptr target_plane = planes[j];
        if (focused_plane->angle(*target_plane) < connect_angular_threshold_) {
          // second, check distance
          bool is_near_enough = isNearPointCloud(kdtree, all_clouds[j]);
          if (is_near_enough) {
            connection_map[i].push_back(j);
          }
        }
      }
    }
    std::vector<std::set<int> > cloud_sets;
    buildAllGroupsSetFromGraphMap(connection_map, cloud_sets);
    // build new indices
    std::vector<pcl::PointIndices::Ptr> new_indices;
    std::vector<pcl::ModelCoefficients::Ptr> new_coefficients;
    for (size_t i = 0; i < cloud_sets.size(); i++) {
      pcl::PointIndices::Ptr new_one_indices (new pcl::PointIndices);
      new_coefficients.push_back(all_coefficients[*cloud_sets[i].begin()]);
      for (std::set<int>::iterator it = cloud_sets[i].begin();
           it != cloud_sets[i].end();
           ++it) {
        new_one_indices = addIndices(*new_one_indices, *all_indices[*it]);
      }
      new_indices.push_back(new_one_indices);
    }
    
    // refinement
    std::vector<pcl::ModelCoefficients::Ptr> new_refined_coefficients;
    for (size_t i = 0; i < new_indices.size(); i++) {
      pcl::ModelCoefficients::Ptr refined_coefficients
        = refinement(cloud, new_indices[i], new_coefficients[i]);
      new_refined_coefficients.push_back(refined_coefficients);
    }

    // publish
    ClusterPointIndices new_ros_indices;
    ModelCoefficientsArray new_ros_coefficients;
    PolygonArray new_ros_polygons;
    new_ros_indices.header = cloud_msg->header;
    new_ros_coefficients.header = cloud_msg->header;
    new_ros_polygons.header = cloud_msg->header;
    new_ros_indices.cluster_indices
      = pcl_conversions::convertToROSPointIndices(new_indices, cloud_msg->header);
    new_ros_coefficients.coefficients
      = pcl_conversions::convertToROSModelCoefficients(
        new_refined_coefficients, cloud_msg->header);
    
    for (size_t i = 0; i < new_indices.size(); i++) {
      ConvexPolygon::Ptr convex
        = convexFromCoefficientsAndInliers<PointT>(
          cloud, new_indices[i], new_refined_coefficients[i]);
      geometry_msgs::PolygonStamped polygon;
      polygon.polygon = convex->toROSMsg();
      polygon.header = cloud_msg->header;
      new_ros_polygons.polygons.push_back(polygon);
    }

    pub_indices_.publish(new_ros_indices);
    pub_polygon_.publish(new_ros_polygons);
    pub_coefficients_.publish(new_ros_coefficients);
  }

  pcl::ModelCoefficients::Ptr PlaneConcatenator::refinement(
    pcl::PointCloud<PointT>::Ptr cloud,
    pcl::PointIndices::Ptr indices,
    pcl::ModelCoefficients::Ptr original_coefficients)
  {
    pcl::SACSegmentation<PointT> seg;
    seg.setOptimizeCoefficients (true);
    seg.setModelType (pcl::SACMODEL_PERPENDICULAR_PLANE);
    seg.setMethodType (pcl::SAC_RANSAC);
    seg.setDistanceThreshold (ransac_refinement_outlier_threshold_);
    
    seg.setInputCloud(cloud);
    
    seg.setIndices(indices);
    seg.setMaxIterations (ransac_refinement_max_iteration_);
    Eigen::Vector3f normal (original_coefficients->values[0],
                            original_coefficients->values[1],
                            original_coefficients->values[2]);
    seg.setAxis(normal);
    // seg.setInputNormals(cloud);
    // seg.setDistanceFromOrigin(original_coefficients->values[3]);
    // seg.setEpsDist(ransac_refinement_eps_distance_);
    seg.setEpsAngle(ransac_refinement_eps_angle_);
    pcl::PointIndices::Ptr refined_inliers (new pcl::PointIndices);
    pcl::ModelCoefficients::Ptr refined_coefficients(new pcl::ModelCoefficients);
    seg.segment(*refined_inliers, *refined_coefficients);
    if (refined_inliers->indices.size() > 0) {
      return refined_coefficients;
    }
    else {
      return original_coefficients;
    }
  }
  
  bool PlaneConcatenator::isNearPointCloud(
    pcl::KdTreeFLANN<PointT>& kdtree,
    pcl::PointCloud<PointT>::Ptr cloud)
  {
    for (size_t i = 0; i < cloud->points.size(); i++) {
      PointT p = cloud->points[i];
      std::vector<int> k_indices;
      std::vector<float> k_sqr_distances;
      if (kdtree.radiusSearch(p, connect_distance_threshold_,
                              k_indices, k_sqr_distances, 1) > 0) {
        return true;
      }
    }
    return false;
  }
    
  void PlaneConcatenator::configCallback(
    Config &config, uint32_t level)
  {
    boost::mutex::scoped_lock lock(mutex_);
    connect_angular_threshold_ = config.connect_angular_threshold;
    connect_distance_threshold_ = config.connect_distance_threshold;
    ransac_refinement_max_iteration_ = config.ransac_refinement_max_iteration;
    ransac_refinement_outlier_threshold_
      = config.ransac_refinement_outlier_threshold;
    ransac_refinement_eps_angle_ = config.ransac_refinement_eps_angle;
    min_size_ = config.min_size;
  }

  void PlaneConcatenator::updateDiagnostic(
    diagnostic_updater::DiagnosticStatusWrapper &stat)
  {
    if (vital_checker_->isAlive()) {
      stat.summary(diagnostic_msgs::DiagnosticStatus::OK,
                   "PlaneConcatenator running");
    }
    else {
      jsk_topic_tools::addDiagnosticErrorSummary(
        "PlaneConcatenator", vital_checker_, stat);
    }
  }

}

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS (jsk_pcl_ros::PlaneConcatenator, nodelet::Nodelet);