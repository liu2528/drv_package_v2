#include <ros/ros.h>

#include <tf2/transform_datatypes.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>
#include <tf2_ros/transform_listener.h>

#include <geometry_msgs/TransformStamped.h>

#include <math.h>
#include <string.h>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/PointCloud2.h>

#include <std_msgs/Bool.h>
#include <std_msgs/UInt16MultiArray.h>
#include <visualization_msgs/Marker.h>

#include <geometry_msgs/PoseStamped.h>

#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>

#include <image_geometry/pinhole_camera_model.h>

#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/sync_policies/exact_time.h>
#include <message_filters/subscriber.h>

// Custom message
#include <drv_msgs/recognized_target.h>
#include "makeplan.h"
#include "getsourcecloud.h"

// 3rd party package
#ifndef USE_CENTER
#include <gpd/GraspConfigList.h>
#endif

//PCL-ROS
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl_conversions/pcl_conversions.h>

//PCL
#include <pcl/filters/extract_indices.h>
#include <pcl/point_types.h>


using namespace std;

typedef pcl::PointCloud<pcl::PointXYZ> PointCloud;

//ros::Publisher graspPubInfo_;

ros::Publisher graspPubStatus_;
ros::Publisher graspPubPose_;

enum ModeType{m_wander, m_search, m_track};
int modeType_ = m_wander;

string param_running_mode = "/status/running_mode";
bool hasGraspPlan_ = false;

bool posePublished_ = false; // merely works in simple mode

// target pointcloud index of image
#ifdef USE_CENTER
int idx_;
int row_;
int col_;
ros::Publisher graspPubMarker_;
ros::Publisher graspPubLocation_;
#else
pcl::PointIndices::Ptr inliers_(new pcl::PointIndices);
ros::Publisher graspPubCloud_;
#endif

// marker type
uint32_t shape = visualization_msgs::Marker::ARROW;

// transform frame
string root_frame_ = "base_link"; // Root frame that NVG link to
string camera_optical_frame_ = "vision_depth_optical_frame";

geometry_msgs::TransformStamped trans_c_;

typedef message_filters::sync_policies::ApproximateTime
        <sensor_msgs::Image, sensor_msgs::CameraInfo> MyApproxSyncDepthPolicy;
message_filters::Synchronizer<MyApproxSyncDepthPolicy> * approxSyncDepth_;

float fx_ = 538.77;
float fy_ = 540.23;
float cx_ = 314.76;
float cy_ = 239.95;

double min_depth_ = 0.3;
double max_depth_ = 3.0;

bool simple_ = false;


void getCloudByInliers(PointCloud::Ptr cloud_in, PointCloud::Ptr &cloud_out,
                       pcl::PointIndices::Ptr inliers, bool negative, bool organized)
{
  pcl::ExtractIndices<pcl::PointXYZ> extract;
  extract.setNegative (negative);
  extract.setInputCloud (cloud_in);
  extract.setIndices (inliers);
  extract.setKeepOrganized(organized);
  extract.filter (*cloud_out);
}

void msgToCloud(const PointCloud::ConstPtr cloud_in, PointCloud::Ptr &cloud_out)
{
  cloud_out->height = cloud_in->height;
  cloud_out->width  = cloud_in->width;
  cloud_out->is_dense = false;
  cloud_out->resize(cloud_out->height * cloud_out->width);
  
  for (size_t i = 0; i < cloud_out->size(); ++i) {
    cloud_out->points[i].x = cloud_in->points[i].x;
    cloud_out->points[i].y = cloud_in->points[i].y;
    cloud_out->points[i].z = cloud_in->points[i].z;
  }
}

void doTransform(PointCloud::Ptr cloud_in, PointCloud::Ptr &cloud_out,
                 const geometry_msgs::TransformStamped& t_in)
{
  Eigen::Transform<float,3,Eigen::Affine> t = Eigen::Translation3f(t_in.transform.translation.x,
                                                                   t_in.transform.translation.y,
                                                                   t_in.transform.translation.z)
      * Eigen::Quaternion<float>(t_in.transform.rotation.w, t_in.transform.rotation.x,
                                 t_in.transform.rotation.y, t_in.transform.rotation.z);
  
  Eigen::Vector3f point;
  
  cloud_out->height = cloud_in->height;
  cloud_out->width  = cloud_in->width;
  cloud_out->is_dense = false;
  cloud_out->resize(cloud_out->height * cloud_out->width);
  
  for (size_t i = 0; i < cloud_in->size(); ++i) {
    point = t * Eigen::Vector3f(cloud_in->points[i].x, cloud_in->points[i].y, cloud_in->points[i].z);
    cloud_out->points[i].x = point.x();
    cloud_out->points[i].y = point.y();
    cloud_out->points[i].z = point.z();
  }
}

void doTransform(pcl::PointXYZ p_in, pcl::PointXYZ &p_out, const geometry_msgs::TransformStamped& t_in)
{
  Eigen::Transform<float,3,Eigen::Affine> t = Eigen::Translation3f(t_in.transform.translation.x,
                                                                   t_in.transform.translation.y,
                                                                   t_in.transform.translation.z)
      * Eigen::Quaternion<float>(t_in.transform.rotation.w, t_in.transform.rotation.x,
                                 t_in.transform.rotation.y, t_in.transform.rotation.z);
  
  Eigen::Vector3f point;
  
  point = t * Eigen::Vector3f(p_in.x, p_in.y, p_in.z);
  p_out.x = point.x();
  p_out.y = point.y();
  p_out.z = point.z();
}

void trackResultCallback(const drv_msgs::recognized_targetConstPtr &msg)
{
  if (modeType_ != m_track)
    return;
  
  posePublished_ = false;
  
#ifdef USE_CENTER
  // directly use center as tgt location
  idx_ = msg->tgt_bbox_center.data[0] + msg->tgt_bbox_center.data[1] * 640;
  row_ = msg->tgt_bbox_center.data[1];
  col_ = msg->tgt_bbox_center.data[0];
#else
  inliers_->indices.clear();
  // use roi to extact point cloud
  for (size_t x = msg->tgt_bbox_array.data[0]; x < msg->tgt_bbox_array.data[2]; x++) {
    for (size_t y = msg->tgt_bbox_array.data[1]; y < msg->tgt_bbox_array.data[3]; y++) {
      int id = y * 640 + x;
      inliers_->indices.push_back(id);
    }
  }
#endif
}

#ifdef USE_CENTER
void depthCallback(
    const sensor_msgs::ImageConstPtr& imageDepth,
    const sensor_msgs::CameraInfoConstPtr& cameraInfo)
{
  // In simple mode, pose only publish once after one detection
  if (modeType_ != m_track || (simple_ && posePublished_))
    return;
  
  if(!(imageDepth->encoding.compare(sensor_msgs::image_encodings::TYPE_16UC1) == 0 ||
       imageDepth->encoding.compare(sensor_msgs::image_encodings::TYPE_32FC1) == 0 ||
       imageDepth->encoding.compare(sensor_msgs::image_encodings::MONO16) == 0))
  {
    ROS_ERROR("Input type must be image_depth=32FC1,16UC1,mono16");
    return;
  }
  
  cv_bridge::CvImageConstPtr imageDepthPtr = cv_bridge::toCvShare(imageDepth);
  
  image_geometry::PinholeCameraModel model;
  model.fromCameraInfo(*cameraInfo);
  fx_ = model.fx();
  fy_ = model.fy();
  cx_ = model.cx();
  cy_ = model.cy();

  MakePlan MP;
  
  pcl::PointXYZ graspPt; // target center in robot's referance frame
  pcl::PointXYZ opticalPt; // target center in camera optical frame

  if (GetSourceCloud::getPoint(imageDepthPtr->image, row_, col_,
                               fx_, fy_, cx_, cy_, max_depth_, min_depth_, opticalPt))
  {
    doTransform(opticalPt, graspPt, trans_c_);
    MP.smartOffset(graspPt, 0.02); //TODO: make this smart

    visualization_msgs::Marker marker;
    // Set the frame ID and timestamp. See the TF tutorials for information on these.
    marker.header.frame_id = root_frame_;
    marker.header.stamp = imageDepth->header.stamp;

    // Set the namespace and id for this marker. This serves to create a unique ID
    // Any marker sent with the same namespace and id will overwrite the old one
    marker.ns = "grasp";
    marker.id = 0;

    marker.type = shape;
    marker.action = visualization_msgs::Marker::ADD;

    // Set the pose of the marker. This is a full 6DOF pose relative to the frame/time specified in the header
    marker.points.resize(2);
    // The point at index 0 is assumed to be the start point, and the point at index 1 is assumed to be the end.
    marker.points[0].x = graspPt.x;
    marker.points[0].y = graspPt.y;
    marker.points[0].z = graspPt.z;

    marker.points[1].x = graspPt.x;
    marker.points[1].y = graspPt.y;
    marker.points[1].z = graspPt.z + 0.15; // arrow height = 0.15m

    //      if we use pose and scale to represent the arrow, remember the original direction is along x axis
    //      marker.scale.x = 0.1;
    //      marker.scale.y = 0.01;
    //      marker.scale.z = 0.01;
    //      marker.pose = ps.pose;

    // scale.x is the shaft diameter, and scale.y is the head diameter. If scale.z is not zero, it specifies the head length.
    // Set the scale of the marker -- 1x1x1 here means 1m on a side
    marker.scale.x = 0.01;
    marker.scale.y = 0.015;
    marker.scale.z = 0.04;

    // Set the color -- be sure to set alpha to something non-zero!
    marker.color.r = 0.0f;
    marker.color.g = 1.0f;
    marker.color.b = 0.0f;
    marker.color.a = 1.0;

    graspPubMarker_.publish(marker);

    //      if (mapFrameExist_)
    //      {
    //        geometry_msgs::PoseStamped location_ps;
    //        location_ps.header.frame_id = "/map";
    //        location_ps.header.stamp = marker.header.stamp;
    //        location_ps.pose.position.x = locationPt.x;
    //        location_ps.pose.position.y = locationPt.y;
    //        location_ps.pose.position.z = locationPt.z;
    //        location_ps.pose.orientation.w = 1; // to rotate x axis to z axis, so the rotation angle = -90 deg
    //        location_ps.pose.orientation.x = 0;
    //        location_ps.pose.orientation.y = 0;
    //        location_ps.pose.orientation.z = 0;
    //        graspPubLocation_.publish(location_ps);
    //      }

    geometry_msgs::PoseStamped grasp_ps;
    grasp_ps.header = marker.header;
    grasp_ps.pose.position.x = graspPt.x;
    grasp_ps.pose.position.y = graspPt.y;
    grasp_ps.pose.position.z = graspPt.z;
    grasp_ps.pose.orientation.w = sqrt(0.5); // to rotate x axis to z axis, so the rotation angle = -90 deg
    grasp_ps.pose.orientation.x = 0;
    grasp_ps.pose.orientation.y = -sqrt(0.5);
    grasp_ps.pose.orientation.z = 0;
    graspPubPose_.publish(grasp_ps);

    hasGraspPlan_ = true;
    posePublished_ = true;
  }
  else
    ROS_ERROR("Get grasp point failed!\n");
}
#else
void sourceCloudCallback(const sensor_msgs::PointCloud2ConstPtr &msg)
{
  // In simple mode, pose only publish once after one detection
  if (modeType_ != m_track || (simple_ && posePublished_))
    return;

  if (inliers_->indices.empty()) {
    ROS_ERROR_THROTTLE(5, "Target ROI contains no point.");
    return;
  }
  // Convert point clound inliers into base frame and publish
  PointCloud::Ptr cloud_in(new PointCloud);
  PointCloud::Ptr cloud_out(new PointCloud);
  pcl::fromROSMsg(*msg, *cloud_in);
  getCloudByInliers(cloud_in, cloud_out, inliers_, false, false);

  // Publish filted cloud for gpd
  sensor_msgs::PointCloud2 rosCloud;
  pcl::toROSMsg(*cloud_out, rosCloud);
  rosCloud.header = msg->header;

  graspPubCloud_.publish(rosCloud);
}

void graspPlanCallback(const gpd::GraspConfigListConstPtr &msg)
{
  // In simple mode, pose only publish once after one detection
  if (modeType_ != m_track || (simple_ && posePublished_))
    return;

  if (msg->grasps.size() == 0) {
    ROS_ERROR("Get grasp plan failed!\n");
    return;
  }

  gpd::GraspConfig grasp;
  for (size_t i = 0; i < msg->grasps.size(); ++i) {
    grasp = msg->grasps[i];

    geometry_msgs::PoseStamped grasp_pose;
    grasp_pose.header = msg->header;
    grasp_pose.pose.position.x = grasp.bottom.x; // TODO: Is using bottom right?
    grasp_pose.pose.position.y = grasp.bottom.y;
    grasp_pose.pose.position.z = grasp.bottom.z;
    grasp_pose.pose.orientation.w = sqrt(0.5); // to rotate x axis to z axis, so the rotation angle = -90 deg
    grasp_pose.pose.orientation.x = 0;
    grasp_pose.pose.orientation.y = -sqrt(0.5);
    grasp_pose.pose.orientation.z = 0;
    graspPubPose_.publish(grasp_pose);

    hasGraspPlan_ = true;
    posePublished_ = true;
    break; // Only publish the first plan
  }
}

#endif

int main(int argc, char **argv)
{
  ros::init(argc, argv, "drv_grasp");
  
  ros::NodeHandle nh;
  ros::NodeHandle pnh("~");
  
  pnh.getParam("root_frame_id", root_frame_);
  pnh.getParam("camera_optical_frame_id", camera_optical_frame_);
  pnh.getParam("simple_id", simple_);
  
  graspPubStatus_ = nh.advertise<std_msgs::Bool>("status/grasp/feedback", 1);
  graspPubPose_ = nh.advertise<geometry_msgs::PoseStamped>("grasp/pose", 1);
  
  tf2_ros::Buffer tfBufferC_;
  tf2_ros::TransformListener tfListenerC_(tfBufferC_);
  tf2_ros::Buffer tfBufferM_;
  tf2_ros::TransformListener tfListenerM_(tfBufferM_);
  
  string topic;
//  if (simple_) {
//    // In simple mode, only respond for search result or user select
//    topic = "search/recognized_target";
//  }
//  else {
    // Otherwise, respond for track result
    topic = "track/recognized_target";
//  }
  ros::Subscriber sub_track = nh.subscribe<drv_msgs::recognized_target>(topic, 1, trackResultCallback);
  
#ifdef USE_CENTER
  graspPubMarker_ = nh.advertise<visualization_msgs::Marker>("grasp/marker", 1);
  graspPubLocation_ = nh.advertise<geometry_msgs::PoseStamped>("grasp/location", 1);

  // sub images to get point coordinate
  int queueSize = 3;
  image_transport::SubscriberFilter imageDepthSub_;
  message_filters::Subscriber<sensor_msgs::CameraInfo> cameraInfoSub_;
  
  ros::NodeHandle depth_nh(nh, "depth");
  ros::NodeHandle depth_pnh(pnh, "depth");
  image_transport::ImageTransport depth_it(depth_nh);
  image_transport::TransportHints hintsDepth("compressedDepth", ros::TransportHints(), depth_pnh);
  
  imageDepthSub_.subscribe(depth_it, depth_nh.resolveName("image_rect"), 1, hintsDepth);
  cameraInfoSub_.subscribe(depth_nh, "camera_info", 1);
  
  approxSyncDepth_ = new message_filters::Synchronizer<MyApproxSyncDepthPolicy>(
        MyApproxSyncDepthPolicy(queueSize), imageDepthSub_, cameraInfoSub_);
  approxSyncDepth_->registerCallback(depthCallback);
#else
  graspPubCloud_ = nh.advertise<sensor_msgs::PointCloud2>("grasp/points", 1);
  ros::Subscriber sub_cloud = nh.subscribe("depth_registered/points", 1, sourceCloudCallback);
  ros::Subscriber sub_grasp_plan = nh.subscribe<gpd::GraspConfigList>("/detect_grasps/clustered_grasps", 1, graspPlanCallback);
#endif
  
  ROS_INFO("Grasp planning function initialized!\n");
  
  while (ros::ok()) {
    if (ros::param::has(param_running_mode))
      ros::param::get(param_running_mode, modeType_);

    std_msgs::Bool flag;
    flag.data = true;
    
    if (modeType_ != m_track) {
      hasGraspPlan_ = false;
      posePublished_ = false;
      continue;
    }
    
    try {
      // the 1st frame is the base frame for transform
      trans_c_ = tfBufferC_.lookupTransform(root_frame_, camera_optical_frame_, ros::Time(0));
    }
    catch (tf2::TransformException &ex) {
      ROS_WARN("%s",ex.what());
      ros::Duration(1.0).sleep();
      continue;
    }
    
    ros::spinOnce();
    
    flag.data = hasGraspPlan_;
    graspPubStatus_.publish(flag);
  }
  
  return 0;
}
