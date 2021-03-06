/*
 * Make sure this line is in your ~/.bashrc:
 *
 * export DRV=/home/USER/catkin_ws/src/drv_package
 *
 * Change 'USER' according to your environment
*/

#include <ros/ros.h>

#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/CameraInfo.h>
#include <std_msgs/Bool.h>
#include <std_msgs/UInt16MultiArray.h>

//custom message
#include <drv_msgs/recognized_target.h>

//stl
#include <cstdlib>
#include <math.h>

#include "goturn.h"

const int angle_step = 1;
const float x_to_angle = 0.02; // a reasonable speed
const float y_to_angle = 0.02;

using namespace std;

// wait loop befor target lost
#define WAIT_LOOP 50
int delay_ = WAIT_LOOP;

image_transport::Publisher trackPubImage_;
ros::Publisher trackPubServo_;
ros::Publisher trackPubStatus_;
ros::Publisher trackPubTarget_; // notice the topic is different from the subscribed one

cv_bridge::CvImageConstPtr src_;
cv::Mat src_img_;

cv_bridge::CvImagePtr track_ptr_(new cv_bridge::CvImage());
cv::Mat track_img_;

enum ModeType{m_wander, m_search, m_track};
int modeType_ = m_wander;
int modeTypeTemp_ = m_wander;

string param_running_mode = "/status/running_mode";
bool isInTracking_ = true;// cause the first call for tracking means have something to track

// Initialize tracking function class
char* drv_path_env = std::getenv("DRV");
std::string drv_path = std::string(drv_path_env);
string test_proto = drv_path + "/supplements/object_track/tracker.prototxt";
string caffe_model  = drv_path + "/supplements/object_track/tracker.caffemodel";

int gpu_id = 0;
const bool do_train = false;
const bool show_output = false;
Goturn GO(test_proto, caffe_model, gpu_id, do_train, show_output);

// Target infomation
std_msgs::String tgt_label_;
cv::Rect detection_;

// global params that recieve servo angle status
string param_servo_pitch = "/status/servo/pitch";
string param_servo_yaw = "/status/servo/yaw";
int pitch_ = 70;
int yaw_ = 90;


void publishServo(int pitch_angle, int yaw_angle)
{
  std_msgs::UInt16MultiArray array;
  array.data.push_back(pitch_angle);
  array.data.push_back(yaw_angle);
  pitch_ = pitch_angle;
  yaw_ = yaw_angle;
  trackPubServo_.publish(array);
}

void servoCallback(const std_msgs::UInt16MultiArrayConstPtr &msg)
{
  // this callback should always active
  pitch_ = msg->data[0];
  yaw_ = msg->data[1];
}

void resultCallback(const drv_msgs::recognized_targetConstPtr &msg)
{
  tgt_label_ = msg->label;
  int min_x = msg->tgt_bbox_array.data[0];
  int min_y = msg->tgt_bbox_array.data[1];
  int max_x = msg->tgt_bbox_array.data[2];
  int max_y = msg->tgt_bbox_array.data[3];
  
  detection_ = cv::Rect(min_x, min_y, max_x - min_x, max_y - min_y);
  GO.tracker_initialized_ = false;
}

bool verifyDetection(cv::Rect detection) {
  if (detection.area() < 20) {
    ROS_WARN("Target area in image is %d, too small to be tracked.\n", detection_.area());
    return false;
  }
  if (detection.x < 0 || detection.x >= 640) {
    ROS_WARN("ROI X is %d.\n", detection_.x);
    return false;
  }
  if (detection.y < 0 || detection.y >= 480) {
    ROS_WARN("ROI Y is %d.\n", detection_.y);
    return false;
  }
  if (detection.x + detection.width >= 640) {
    ROS_WARN("ROI X+W is %d.\n", detection_.x + detection.width);
    return false;
  }
  if (detection.y + detection.height >= 480) {
    ROS_WARN("ROI Y+H is %d.\n", detection_.y + detection.height);
    return false;
  }
  if (detection.width <= 0 || detection.height <= 0) {
    ROS_WARN("ROI W, H is %d, %d.\n", detection.width, detection.height);
    return false;
  }
  return true;
}

void pubTarget(std_msgs::Header header, std::vector<unsigned int> mask_id, cv::Rect roi) {
  // publish new target info
  drv_msgs::recognized_target result;
  result.header = header;
  result.label = tgt_label_;
  result.tgt_pixels.data = mask_id; // the datatype is uint 32 aka unsigned int
  result.tgt_bbox_array.data.push_back(roi.x);
  result.tgt_bbox_array.data.push_back(roi.y);
  result.tgt_bbox_array.data.push_back(roi.x + roi.width);
  result.tgt_bbox_array.data.push_back(roi.y + roi.height);
  result.tgt_bbox_center.data.push_back(roi.x + roi.width / 2);
  result.tgt_bbox_center.data.push_back(roi.y + roi.height / 2);
  
  trackPubTarget_.publish(result);
}

void imageCallback(const sensor_msgs::ImageConstPtr& image_msg)
{
  if (modeType_ != m_track)
    return;
  
  if (!verifyDetection(detection_))
  {
    isInTracking_ = false;
    GO.tracker_initialized_ = false;
    return;
  }
  
  src_ = cv_bridge::toCvShare(image_msg, sensor_msgs::image_encodings::BGR8);
  src_img_ = src_->image;
  src_img_.copyTo(track_img_);
  
  // cv::rectangle(track_image_, detection_, cv::Scalar(232,228,53),2);
  
  cv::Rect roi;
  std::vector<unsigned int> mask_id; // store object pixels id in image
  
  // Use roi to adjust the camera view
  if (GO.goProcess(src_img_, detection_, track_img_, roi, mask_id))
  {
    // publish exact location and boundaries of tracked object
    track_ptr_->header = image_msg->header;
    track_ptr_->image = track_img_;
    track_ptr_->encoding = sensor_msgs::image_encodings::BGR8;
    trackPubImage_.publish(track_ptr_->toImageMsg());
    
    // drive the camera so that the center of the image captured is on object
    int d_x = roi.x + roi.width / 2 - 320;
    int d_y = roi.y + roi.height / 2 - 240;
    int deg_x = int(d_x * x_to_angle); // offset the robot head need to turn
    int deg_y = int(d_y * y_to_angle);
    
    isInTracking_ = true;
    delay_ = WAIT_LOOP;
    pubTarget(image_msg->header, mask_id, roi);
    
    if (abs(deg_x) < angle_step && abs(deg_y) < angle_step) {
      isInTracking_ = true;
      delay_ = WAIT_LOOP;
      pubTarget(image_msg->header, mask_id, roi);
      return;
    }
    if (abs(deg_x) >= angle_step && abs(deg_y) < angle_step) {
      if (deg_x < -angle_step) deg_x = -angle_step;
      if (deg_x > angle_step) deg_x = angle_step;
      deg_y = 0;
    }
    if (abs(deg_x) < angle_step && abs(deg_y) >= angle_step) {
      if (deg_y < -angle_step) deg_y = -angle_step;
      if (deg_y > angle_step) deg_y = angle_step;
      deg_x = 0;
    }
    if (abs(deg_x) >= angle_step && abs(deg_y) >= angle_step) {
      if (deg_x < -angle_step) deg_x = -angle_step;
      if (deg_x > angle_step) deg_x = angle_step;
      if (deg_y < -angle_step) deg_y = -angle_step;
      if (deg_y > angle_step) deg_y = angle_step;
    }
    int x_ang = - deg_x + yaw_;
    int y_ang = - deg_y + pitch_;
    
    if (!(x_ang >= 0 && x_ang <= 180 && y_ang >= 60 && y_ang <= 140))
    {
      pubTarget(image_msg->header, mask_id, roi);
      isInTracking_ = false;
      ROS_WARN("Target out of camera view.\n");
      GO.tracker_initialized_ = false;
    }
    else {
      publishServo(y_ang, x_ang);
    }
  }
  else
  {
    delay_--;
    if (delay_ < 0)
    {
      isInTracking_ = false;
      ROS_WARN("GOTURN lost the target.\n");
      GO.tracker_initialized_ = false;
    }
  }
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "drv_track");
  
  ros::NodeHandle nh;
  ros::NodeHandle pnh;
  ros::NodeHandle rgb_nh(nh, "rgb");
  ros::NodeHandle rgb_pnh(pnh, "rgb");
  
  image_transport::ImageTransport it_rgb_sub(rgb_nh);
  image_transport::TransportHints hints_rgb("compressed", ros::TransportHints(), rgb_pnh);
  
  image_transport::ImageTransport it_rgb_pub(nh);
  trackPubImage_ = it_rgb_pub.advertise("search/labeled_image", 1);
  trackPubServo_ = nh.advertise<std_msgs::UInt16MultiArray>("servo", 1);
  trackPubStatus_ = nh.advertise<std_msgs::Bool>("status/track/feedback", 1);
  trackPubTarget_ = nh.advertise<drv_msgs::recognized_target>("track/recognized_target" , 1);
  
  ros::Subscriber sub_res = nh.subscribe<drv_msgs::recognized_target>("search/recognized_target", 1, resultCallback);
  image_transport::Subscriber sub_rgb = it_rgb_sub.subscribe("image_raw", 1, imageCallback, hints_rgb);
  ros::Subscriber sub_s = nh.subscribe<std_msgs::UInt16MultiArray>("servo", 1, servoCallback);
  
  if (ros::param::has(param_servo_pitch))
    ros::param::get(param_servo_pitch, pitch_);
  
  if (ros::param::has(param_servo_yaw))
    ros::param::get(param_servo_yaw, yaw_);
  
  ROS_INFO("Tracking function initialized!\n");
  
  while (ros::ok())
  {
    if (ros::param::has(param_running_mode))
    {
      ros::param::get(param_running_mode, modeTypeTemp_);
      
      if (modeTypeTemp_ != m_track && modeType_ == m_track)
      {
        GO.tracker_initialized_ = false;
      }
      modeType_ = modeTypeTemp_;
    }
    
    std_msgs::Bool flag;
    flag.data = true;
    
    ros::spinOnce();
    
    flag.data = isInTracking_;
    trackPubStatus_.publish(flag);
  }
  
  return 0;
}

