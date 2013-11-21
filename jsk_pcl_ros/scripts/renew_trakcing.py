#!/usr/bin/env python
import rospy
from sensor_msgs.msg import PointCloud2
from jsk_pcl_ros.srv import SetPointCloud2

def cloud_cb(cloud):

    try:
        setCloud = rospy.ServiceProxy('/pcl_nodelet/particle_filter_tracker/renew_model', SetPointCloud2)
        setCloud(cloud)
        ROS_INFO("Success renew_tracking service");
    except rospy.ServiceException, e:
            ROS_INFO("Failed to call renew_tracking service")

if __name__ == "__main__":
    rospy.init_node('send_renew_model', anonymous=True)
    rospy.wait_for_service('/pcl_nodelet/particle_filter_tracker/renew_model')
    rospy.Subscriber("/selected_pointcloud", PointCloud2, cloud_cb)
    rospy.spin()