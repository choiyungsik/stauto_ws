//
// Created by usl on 4/10/19.
//

#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/CameraInfo.h>
#include <tf/transform_listener.h>
#include <image_transport/image_transport.h>
#include <opencv2/highgui/highgui.hpp>
#include <cv_bridge/cv_bridge.h>
#include <Eigen/Geometry>

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <pcl/common/common.h>
#include <pcl/common/pca.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/io/pcd_io.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl_conversions/pcl_conversions.h>

#include <pcl/filters/passthrough.h>

#include <pcl/sample_consensus/ransac.h>
#include <pcl/sample_consensus/sac_model.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/sac_model_line.h>
#include <pcl/sample_consensus/sac_model_plane.h>
#include <pcl/sample_consensus/sac_model_sphere.h>

#include <pcl/filters/statistical_outlier_removal.h>

#include <opencv2/calib3d.hpp>
#include <opencv2/core/eigen.hpp>

#include <tf/transform_broadcaster.h>
#include <tf_conversions/tf_eigen.h>

#include <iostream>
#include <fstream>
#include<algorithm>


#include "darknet_ros_msgs/BoundingBoxes.h"
#include "darknet_ros_msgs/BoundingBox.h"
#include <visualization_msgs/MarkerArray.h>


typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::PointCloud2,
                                                        sensor_msgs::Image> SyncPolicy;

class lidarImageProjection {
private:

    ros::NodeHandle nh;

    message_filters::Subscriber<sensor_msgs::PointCloud2> *cloud_sub;
    message_filters::Subscriber<sensor_msgs::Image> *image_sub;
    message_filters::Synchronizer<SyncPolicy> *sync;

    ros::Subscriber Bbox_sub;
    ros::Subscriber markers_sub;

    ros::Publisher cloud_pub;
    ros::Publisher image_pub;
    ros::Publisher marker_array_pub;

    cv::Mat c_R_l, tvec;
    cv::Mat rvec;
    std::string result_str;
    Eigen::Matrix4d C_T_L, L_T_C;
    Eigen::Matrix3d C_R_L, L_R_C;
    Eigen::Quaterniond C_R_L_quatn, L_R_C_quatn;
    Eigen::Vector3d C_t_L, L_t_C;

    bool project_only_plane;
    cv::Mat projection_matrix;
    cv::Mat distCoeff;

    std::vector<cv::Point3d> objectPoints_L, objectPoints_C;
    std::vector<cv::Point2d> imagePoints;

    sensor_msgs::PointCloud2 out_cloud_ros;

    std::string lidar_frameId;

    std::string camera_in_topic;
    std::string lidar_in_topic;

    pcl::PointCloud<pcl::PointXYZRGB> out_cloud_pcl;
    cv::Mat image_in;

    int dist_cut_off;


    int xmin;
    int xmax;
    int ymin;
    int ymax;
    int object_num;

    int delay_num;
    int row;
    int column;

    std::vector<std::vector<int>> bb_arr;
    std::vector<std::vector<float>> obj_cor;
    std::vector<std::string> label_vec;

    visualization_msgs::MarkerArray markers;
    visualization_msgs::MarkerArray markers_name;

    std::string cam_config_file_path;
    int image_width, image_height;

    std::string camera_name;

public:
    lidarImageProjection() {
        camera_in_topic = readParam<std::string>(nh, "camera_in_topic");
        lidar_in_topic = readParam<std::string>(nh, "lidar_in_topic");
        dist_cut_off = readParam<int>(nh, "dist_cut_off");
        camera_name = readParam<std::string>(nh, "camera_name");
        cloud_sub =  new message_filters::Subscriber<sensor_msgs::PointCloud2>(nh, lidar_in_topic, 1);
        image_sub = new message_filters::Subscriber<sensor_msgs::Image>(nh, camera_in_topic, 1);
        std::string lidarOutTopic = camera_in_topic + "/velodyne_out_cloud";
        cloud_pub = nh.advertise<sensor_msgs::PointCloud2>(lidarOutTopic, 1);
        std::string imageOutTopic = camera_in_topic + "/projected_image";
        image_pub = nh.advertise<sensor_msgs::Image>(imageOutTopic, 1);
        marker_array_pub = nh.advertise<visualization_msgs::MarkerArray>("projection/markers", 100);

        Bbox_sub = nh.subscribe("/darknet_ros/bounding_boxes", 10, &lidarImageProjection::Bbox_callback,this);
        markers_sub = nh.subscribe("/adaptive_clustering/markers", 10, &lidarImageProjection::marker_array_callback,this);

        sync = new message_filters::Synchronizer<SyncPolicy>(SyncPolicy(10), *cloud_sub, *image_sub);
        sync->registerCallback(boost::bind(&lidarImageProjection::callback, this, _1, _2));

        C_T_L = Eigen::Matrix4d::Identity();
        c_R_l = cv::Mat::zeros(3, 3, CV_64F);
        tvec = cv::Mat::zeros(3, 1, CV_64F);

        result_str = readParam<std::string>(nh, "result_file");
        project_only_plane = readParam<bool>(nh, "project_only_plane");

        projection_matrix = cv::Mat::zeros(3, 3, CV_64F);
        distCoeff = cv::Mat::zeros(5, 1, CV_64F);

        std::ifstream myReadFile(result_str.c_str());
        std::string word;
        int i = 0;
        int j = 0;
        while (myReadFile >> word){
            C_T_L(i, j) = atof(word.c_str());
            j++;
            if(j>3) {
                j = 0;
                i++;
            }
        }
        L_T_C = C_T_L.inverse();

        C_R_L = C_T_L.block(0, 0, 3, 3);
        C_t_L = C_T_L.block(0, 3, 3, 1);

        L_R_C = L_T_C.block(0, 0, 3, 3);
        L_t_C = L_T_C.block(0, 3, 3, 1);

        cv::eigen2cv(C_R_L, c_R_l);
        C_R_L_quatn = Eigen::Quaterniond(C_R_L);
        L_R_C_quatn = Eigen::Quaterniond(L_R_C);
        cv::Rodrigues(c_R_l, rvec);
        cv::eigen2cv(C_t_L, tvec);

        cam_config_file_path = readParam<std::string>(nh, "cam_config_file_path");
        readCameraParams(cam_config_file_path,
                         image_height,
                         image_width,
                         distCoeff,
                         projection_matrix);

        row = 20;
        delay_num = 0;

        for (int i = 0; i < row; i++) {
            std::vector<int> element(4,0);
            bb_arr.push_back(element);
        }

        for (int i = 0; i < row; i++) {
            std::vector<float> element(4,0);
            obj_cor.push_back(element);
        }
        
        for (int i = 0; i < row; i++) {
            label_vec.push_back("");
        }
    }

    void readCameraParams(std::string cam_config_file_path,
                          int &image_height,
                          int &image_width,
                          cv::Mat &D,
                          cv::Mat &K) {
        cv::FileStorage fs_cam_config(cam_config_file_path, cv::FileStorage::READ);
        if(!fs_cam_config.isOpened())
            std::cerr << "Error: Wrong path: " << cam_config_file_path << std::endl;
        fs_cam_config["image_height"] >> image_height;
        fs_cam_config["image_width"] >> image_width;
        fs_cam_config["k1"] >> D.at<double>(0);
        fs_cam_config["k2"] >> D.at<double>(1);
        fs_cam_config["p1"] >> D.at<double>(2);
        fs_cam_config["p2"] >> D.at<double>(3);
        fs_cam_config["k3"] >> D.at<double>(4);
        fs_cam_config["fx"] >> K.at<double>(0, 0);
        fs_cam_config["fy"] >> K.at<double>(1, 1);
        fs_cam_config["cx"] >> K.at<double>(0, 2);
        fs_cam_config["cy"] >> K.at<double>(1, 2);
    }

    template <typename T>
    T readParam(ros::NodeHandle &n, std::string name){
        T ans;
        if (n.getParam(name, ans)){
            ROS_INFO_STREAM("Loaded " << name << ": " << ans);
        } else {
            ROS_ERROR_STREAM("Failed to load " << name);
            n.shutdown();
        }
        return ans;
    }

    pcl::PointCloud<pcl::PointXYZ >::Ptr planeFilter(const sensor_msgs::PointCloud2ConstPtr &cloud_msg) {

        pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::fromROSMsg(*cloud_msg, *in_cloud);

        pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered_x(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered_y(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::PointXYZ >::Ptr plane(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::PointXYZ >::Ptr plane_filtered(new pcl::PointCloud<pcl::PointXYZ>);

        /// Pass through filters
        pcl::PassThrough<pcl::PointXYZ> pass_x;
        pass_x.setInputCloud(in_cloud);
        pass_x.setFilterFieldName("x");
        pass_x.setFilterLimits(0.0, 5.0);
        pass_x.filter(*cloud_filtered_x);
        pcl::PassThrough<pcl::PointXYZ> pass_y;
        pass_y.setInputCloud(cloud_filtered_x);
        pass_y.setFilterFieldName("y");
        pass_y.setFilterLimits(-1.25, 1.25);
        pass_y.filter(*cloud_filtered_y);

        /// Plane Segmentation
        pcl::SampleConsensusModelPlane<pcl::PointXYZ>::Ptr model_p(
                new pcl::SampleConsensusModelPlane<pcl::PointXYZ>(cloud_filtered_y));
        pcl::RandomSampleConsensus<pcl::PointXYZ> ransac(model_p);
        ransac.setDistanceThreshold(0.01);
        ransac.computeModel();
        std::vector<int> inliers_indicies;
        ransac.getInliers(inliers_indicies);
        pcl::copyPointCloud<pcl::PointXYZ>(*cloud_filtered_y, inliers_indicies, *plane);

        /// Statistical Outlier Removal
        pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
        sor.setInputCloud(plane);
        sor.setMeanK (50);
        sor.setStddevMulThresh (1);
        sor.filter (*plane_filtered);

        return plane_filtered;
    }

    cv::Vec3b atf(cv::Mat rgb, cv::Point2d xy_f){
        cv::Vec3i color_i;
        color_i.val[0] = color_i.val[1] = color_i.val[2] = 0;

        int x = xy_f.x;
        int y = xy_f.y;
        
        object_num = -1;
        int x_center = 0;
        int y_center = 0;
        int width = 0;
        int height = 0;

        for (int row = 0; row <= 1; row++){
            for (int col = 0; col <= 1; col++){
                if((x+col)< rgb.cols && (y+row) < rgb.rows) {
                    for(int num = 0; num < label_vec.size(); num++) {
                        x_center = (bb_arr[num][0] + bb_arr[num][1]) / 2;
                        y_center = (bb_arr[num][2] + bb_arr[num][3]) / 2;
                        width  = bb_arr[num][1] - bb_arr[num][0];
                        height = bb_arr[num][3] - bb_arr[num][2];


                        // if(x_center < 5) {
                        //     x_center = 5;
                        // }

                        // else if(x_center > 715) {
                        //     x_center = 715;
                        // }

                        // if(y_center < 5) {
                        //     y_center = 5;
                        // }

                        // else if(y_center > 1275) {
                        //     y_center = 1275;
                        // }
                        // if(bb_arr[num][0] + 10 <= x + col && x + col <= bb_arr[num][1] - 10 && bb_arr[num][2] + 10 <= y + row && y + row <= bb_arr[num][2] - 10) {
                        //     object_num = num;
                        //     std::cout << object_num <<std::endl;
                        // }

                        if(bb_arr[num][0] <= x + col && x + col <= bb_arr[num][1] && bb_arr[num][2] <= y + row && y + row<= bb_arr[num][3]) {
                            object_num = num;
                            // std::cout << object_num <<std::endl;
                        }

                        // if(width/2 < 30 || height/2 < 30) {
                        //     std::cout << "out of boarder!!!!!!!!!!!!!!!!!!!!" << std::endl;
                        // }

                        // if(x_center -30 <= x + col && x + col <= x_center + 30 && y_center -30 <= y + row && y + row<= y_center + 30) {
                        //     object_num = num;
                        //     // std::cout << object_num <<std::endl;
                        // }   // center -+ 30 size bbox


                    }
                    // cv::Vec3b c = rgb.at<cv::Vec3b>(cv::Point(x + col, y + row));
                    // for (int i = 0; i < 3; i++){
                    //     color_i.val[i] += c.val[i];
                    // }

                    if(object_num >= 0 ){
                        // std::cout << xmin << "   " << x + col << std::endl;
                        cv::Vec3b c = rgb.at<cv::Vec3b>(cv::Point(x + col, y + row));
                        for (int i = 0; i < 3; i++){
                            if(i == 2){
                                color_i.val[i] += 255;   
                            }
                            else{
                                color_i.val[i] += 0;   
                            }
                        }
                    }

                    else{
                        cv::Vec3b c = rgb.at<cv::Vec3b>(cv::Point(x + col, y + row));
                        for (int i = 0; i < 3; i++){
                            //color_i.val[i] += 0;
                            color_i.val[i] += 0;
                        }
                    }
                }
            }
        }

        cv::Vec3b color;
        for (int i = 0; i < 3; i++){
            color.val[i] = color_i.val[i] / 4;
        }
        return color;
    }

    void publishTransforms() {
        static tf::TransformBroadcaster br;
        tf::Transform transform;
        tf::Quaternion q;
        tf::quaternionEigenToTF(L_R_C_quatn, q);
        transform.setOrigin(tf::Vector3(L_t_C(0), L_t_C(1), L_t_C(2)));
        transform.setRotation(q);
        br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), lidar_frameId, camera_name));
    }

    void colorPointCloud() {
        tf::TransformBroadcaster br;
        tf::Transform transform;
        tf::Quaternion q;
        visualization_msgs::Marker marker;
        q.setRPY(0, 0, 0);
        transform.setRotation(q);

        std::vector<float> marker_cor_x;
        std::vector<float> marker_cor_y;
        std::vector<float> marker_cor_z;
        std::vector<float> marker_cor_x_len;
        std::vector<float> marker_cor_y_len;
        std::vector<float> marker_cor_z_len;
        std::vector<std::vector<int>> label_num_vec;

        float x_len = 0;
        float y_len = 0;
        float z_len = 0;
        float x = 0;
        float y = 0;
        float z = 0;
        
        bool duplicated = false;

        out_cloud_pcl.points.clear();
        out_cloud_pcl.resize(objectPoints_L.size());

        for(int j=0; j < markers.markers.size(); j++) {
            std::vector<int> element(20,0);
            label_num_vec.push_back(element);

            x_len = std::abs(markers.markers[j].points[0].x - markers.markers[j].points[1].x);
            y_len = std::abs(markers.markers[j].points[0].y - markers.markers[j].points[3].y);
            z_len = std::abs(markers.markers[j].points[0].z - markers.markers[j].points[5].z);
            x = (markers.markers[j].points[0].x + markers.markers[j].points[1].x) / 2;
            y = (markers.markers[j].points[0].y + markers.markers[j].points[3].y) / 2;
            z = (markers.markers[j].points[0].z + markers.markers[j].points[5].z) / 2;

            marker_cor_x.push_back(x);
            marker_cor_y.push_back(y);
            marker_cor_z.push_back(z);
            marker_cor_x_len.push_back(x_len);
            marker_cor_y_len.push_back(y_len);
            marker_cor_z_len.push_back(z_len);
        }

        for(size_t i = 0; i < objectPoints_L.size(); i++) {
            // std::cout << imagePoints[i] << std::endl;
            // std::cout << "asd" << object_num <<std::endl;
            cv::Vec3b rgb = atf(image_in, imagePoints[i]);
            pcl::PointXYZRGB pt_rgb(rgb.val[2], rgb.val[1], rgb.val[0]);
            pt_rgb.x = objectPoints_L[i].x;
            pt_rgb.y = objectPoints_L[i].y;
            pt_rgb.z = objectPoints_L[i].z;
        
            if(object_num >= 0) {
                for(int j=0; j < markers.markers.size(); j++) {
                    if(marker_cor_x[j]-marker_cor_x_len[j]/2 <= pt_rgb.x && pt_rgb.x <= marker_cor_x[j]+marker_cor_x_len[j]/2 && marker_cor_y[j]-marker_cor_y_len[j]/2 <= pt_rgb.y && pt_rgb.y <= marker_cor_y[j]+marker_cor_y_len[j]/2 && marker_cor_z[j]-marker_cor_z_len[j]/2 <= pt_rgb.z && pt_rgb.z <= marker_cor_z[j]+marker_cor_z_len[j]/2) {
                        label_num_vec[j][object_num] += 1;
                    }
                }
            }   
            out_cloud_pcl.push_back(pt_rgb);
        }
        int marker_id = 0;
        for(int j=0; j < markers.markers.size(); j++) {
            int maxElementIndex = std::max_element(label_num_vec[j].begin(),label_num_vec[j].end()) - label_num_vec[j].begin();
            if(label_num_vec[j][maxElementIndex] != 0) {
                marker.header.frame_id = "/velodyne";
                marker.header.stamp = ros::Time::now();
                marker.ns = "basic_shapes";
                marker.id = marker_id;
                marker.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
                marker.action = visualization_msgs::Marker::ADD;

                marker.pose.position.x = marker_cor_x[j];
                marker.pose.position.y = marker_cor_y[j];
                marker.pose.position.z = marker_cor_z[j];

                marker.pose.orientation.x = 0.0;
                marker.pose.orientation.y = 0.0;
                marker.pose.orientation.z = 0.0;
                marker.pose.orientation.w = 1.0;

                marker.scale.x = 0.4;
                marker.scale.y = 0.4;
                marker.scale.z = 0.4;

                marker.color.r = 1.0f;
                marker.color.g = 0.0f;
                marker.color.b = 0.0f;
                marker.color.a = 1.0;

                marker.lifetime = ros::Duration(1);

                marker.text = label_vec[maxElementIndex] + "_" + std::to_string(label_num_vec[j][maxElementIndex]);
                
                markers_name.markers.push_back(marker);
                marker_id++;
            }

        }

        if(markers_name.markers.size() > 0) {
            marker_array_pub.publish(markers_name);
        }
           
        markers_name.markers.clear();    
        marker_cor_x.clear();  
        marker_cor_y.clear();
        marker_cor_z.clear();  
        marker_cor_x_len.clear();
        marker_cor_y_len.clear();
        marker_cor_z_len.clear();
    }

    void colorLidarPointsOnImage(double min_range,
            double max_range) {
        for(size_t i = 0; i < imagePoints.size(); i++) {
            double X = objectPoints_C[i].x;
            double Y = objectPoints_C[i].y;
            double Z = objectPoints_C[i].z;
            double range = sqrt(X*X + Y*Y + Z*Z);
            double red_field = 255*(range - min_range)/(max_range - min_range);
            double green_field = 255*(max_range - range)/(max_range - min_range);
            cv::circle(image_in, imagePoints[i], 2,
                       CV_RGB(red_field, green_field, 0), -1, 1, 0);
        }
    }

    void callback(const sensor_msgs::PointCloud2ConstPtr &cloud_msg,
                  const sensor_msgs::ImageConstPtr &image_msg) {
        lidar_frameId = cloud_msg->header.frame_id;
        objectPoints_L.clear();
        objectPoints_C.clear();
        imagePoints.clear();
        publishTransforms();
        image_in = cv_bridge::toCvShare(image_msg, "bgr8")->image;

        double fov_x, fov_y;
        fov_x = 2*atan2(image_width, 2*projection_matrix.at<double>(0, 0))*180/CV_PI;
        fov_y = 2*atan2(image_height, 2*projection_matrix.at<double>(1, 1))*180/CV_PI;

        double max_range, min_range;
        max_range = -INFINITY;
        min_range = INFINITY;

        pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        if(project_only_plane) {
            in_cloud = planeFilter(cloud_msg);
            for(size_t i = 0; i < in_cloud->points.size(); i++) {
                objectPoints_L.push_back(cv::Point3d(in_cloud->points[i].x, in_cloud->points[i].y, in_cloud->points[i].z));
            }
            cv::projectPoints(objectPoints_L, rvec, tvec, projection_matrix, distCoeff, imagePoints, cv::noArray());
        } else {
            pcl::PCLPointCloud2 *cloud_in = new pcl::PCLPointCloud2;
            pcl_conversions::toPCL(*cloud_msg, *cloud_in);
            pcl::fromPCLPointCloud2(*cloud_in, *in_cloud);

            for(size_t i = 0; i < in_cloud->points.size(); i++) {

                // Reject points behind the LiDAR(and also beyond certain distance)
                if(in_cloud->points[i].x < 0 || in_cloud->points[i].x > dist_cut_off)
                    continue;

                Eigen::Vector4d pointCloud_L;
                pointCloud_L[0] = in_cloud->points[i].x;
                pointCloud_L[1] = in_cloud->points[i].y;
                pointCloud_L[2] = in_cloud->points[i].z;
                pointCloud_L[3] = 1;

                Eigen::Vector3d pointCloud_C;
                pointCloud_C = C_T_L.block(0, 0, 3, 4)*pointCloud_L;

                double X = pointCloud_C[0];
                double Y = pointCloud_C[1];
                double Z = pointCloud_C[2];

                double Xangle = atan2(X, Z)*180/CV_PI;
                double Yangle = atan2(Y, Z)*180/CV_PI;

                if(Xangle < -fov_x/2 || Xangle > fov_x/2)
                    continue;

                if(Yangle < -fov_y/2 || Yangle > fov_y/2)
                    continue;

                double range = sqrt(X*X + Y*Y + Z*Z);

                if(range > max_range) {
                    max_range = range;
                }
                if(range < min_range) {
                    min_range = range;
                }

                objectPoints_L.push_back(cv::Point3d(pointCloud_L[0], pointCloud_L[1], pointCloud_L[2]));
                objectPoints_C.push_back(cv::Point3d(X, Y, Z));
            }
            cv::projectPoints(objectPoints_L, rvec, tvec, projection_matrix, distCoeff, imagePoints, cv::noArray());
        }

        /// Color the Point Cloud
        colorPointCloud();

        pcl::toROSMsg(out_cloud_pcl, out_cloud_ros);
        out_cloud_ros.header.frame_id = cloud_msg->header.frame_id;
        out_cloud_ros.header.stamp = cloud_msg->header.stamp;

        cloud_pub.publish(out_cloud_ros);

        /// Color Lidar Points on the image a/c to distance
        colorLidarPointsOnImage(min_range, max_range);

        sensor_msgs::ImagePtr msg =
                cv_bridge::CvImage(std_msgs::Header(), "bgr8", image_in).toImageMsg();
        image_pub.publish(msg);

        delay_num += 1;

        label_vec.clear();
        for (int i = 0; i < row; i++) {
                label_vec.push_back("");
            }
        bb_arr.clear();
        for (int i = 0; i < row; i++) {
            std::vector<int> element(4,0);
            bb_arr.push_back(element);
        }
        
        delay_num = 0;
        
        
//        cv::Mat image_resized;
//        cv::resize(lidarPtsImg, image_resized, cv::Size(), 0.25, 0.25);
//        cv::imshow("view", image_resized);
//        cv::waitKey(10);
    }

    void Bbox_callback(const darknet_ros_msgs::BoundingBoxes::ConstPtr& msg){
        
        for(int i=0; i < msg->bounding_boxes.size(); i++) {
            // if(msg->bounding_boxes[i].Class == "person") {
                bb_arr[i][0] = msg->bounding_boxes[i].xmin; 
                bb_arr[i][1] = msg->bounding_boxes[i].xmax;
                bb_arr[i][2] = msg->bounding_boxes[i].ymin;
                bb_arr[i][3] = msg->bounding_boxes[i].ymax;
                label_vec[i] = msg->bounding_boxes[i].Class;

            // }
        }

        for(int i=0; i < label_vec.size(); i++) {
            if(label_vec[i].find("_") == std::string::npos && label_vec[i] != "") {
                label_vec[i] = label_vec[i] + "_" + std::to_string(i);
            }
        }

    }

    void marker_array_callback(const visualization_msgs::MarkerArray::ConstPtr& msg) {
        markers = *msg;

    }

    
};

int main(int argc, char** argv) {
    ros::init(argc, argv, "cam_lidar_proj");
    lidarImageProjection lip;
    ros::spin();
    return 0;
}