//
// The MIT License (MIT)
//
// Copyright (c) 2019 Livox. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include "lddc.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>

#include <pcl_ros/point_cloud.h>
#include <ros/ros.h>
#include <rosbag/bag.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>

#include "lds_lidar.h"
#include "lds_lvx.h"
#include <livox_ros_driver/CustomMsg.h>
#include <livox_ros_driver/CustomPoint.h>

namespace livox_ros {

typedef pcl::PointCloud<pcl::PointXYZI> PointCloud;

/** Lidar Data Distribute Control--------------------------------------------*/
Lddc::Lddc(int format, int multi_topic, int data_src, int output_type,
           double frq, std::string &frame_id)
    : transfer_format_(format), use_multi_topic_(multi_topic),
      data_src_(data_src), output_type_(output_type), publish_frq_(frq), \
      frame_id_(frame_id) {
  publish_period_ns_ = kNsPerSecond / publish_frq_;
  lds_ = nullptr;
  memset(private_pub_, 0, sizeof(private_pub_));
  memset(private_imu_pub_, 0, sizeof(private_imu_pub_));
  global_pub_ = nullptr;
  global_imu_pub_ = nullptr;
  cur_node_ = nullptr;
  bag_ = nullptr;
};

Lddc::~Lddc() {

  if (global_pub_) {
    delete global_pub_;
  }

  if (global_imu_pub_) {
    delete global_pub_;
  }

  if (lds_) {
    lds_->PrepareExit();
  }

  for (uint32_t i = 0; i < kMaxSourceLidar; i++) {
    if (private_pub_[i]) {
      delete private_pub_[i];
    }
  }

  for (uint32_t i = 0; i < kMaxSourceLidar; i++) {
    if (private_imu_pub_[i]) {
      delete private_imu_pub_[i];
    }
  }
}

int32_t Lddc::GetPublishStartTime(LidarDevice *lidar, LidarDataQueue *queue,
    uint64_t *start_time,StoragePacket *storage_packet) {
  QueueProPop(queue, storage_packet);
  uint64_t timestamp = GetStoragePacketTimestamp(storage_packet, 
                                                 lidar->data_src);
  uint32_t remaining_time = timestamp % publish_period_ns_;
  uint32_t diff_time = publish_period_ns_ - remaining_time;
  /** Get start time, down to the period boundary */
  if (diff_time > (publish_period_ns_ / 4)) {
    //ROS_INFO("0 : %u", diff_time);
    *start_time = timestamp - remaining_time;
    return 0;
  } else if (diff_time <= lidar->packet_interval_max) {
    *start_time = timestamp;
    return 0;
  } else {
    /** Skip some packets up to the period boundary*/ 
    //ROS_INFO("2 : %u", diff_time);
    do {
      if (QueueIsEmpty(queue)) {
        break;
      }
      QueuePopUpdate(queue); /* skip packet */      
      QueueProPop(queue, storage_packet);
      uint32_t last_remaning_time = remaining_time;
      timestamp = GetStoragePacketTimestamp(storage_packet, 
                                            lidar->data_src);
      remaining_time = timestamp % publish_period_ns_;
      /** Flip to another period */
      if (last_remaning_time > remaining_time) {
        //ROS_INFO("Flip to another period, exit");
        break;
      }
      diff_time = publish_period_ns_ - remaining_time;
    } while (diff_time > lidar->packet_interval);
      
    /* the remaning packets in queue maybe not enough after skip */
    return -1;
  }
}

/* @Wei, penultimate level to publish the pointcloud from queue */
uint32_t Lddc::PublishPointcloud2(LidarDataQueue *queue, uint32_t packet_num,
                                  uint8_t handle) {
  uint64_t timestamp = 0;
  uint64_t last_timestamp = 0;
  uint32_t published_packet = 0;

  StoragePacket storage_packet;
  LidarDevice *lidar  = &lds_->lidars_[handle];
  if (GetPublishStartTime(lidar, queue, &last_timestamp, &storage_packet)) {
    /* the remaning packets in queue maybe not enough after skip */
    return 0;
  }
  
  sensor_msgs::PointCloud2 cloud;
  cloud.header.frame_id.assign(frame_id_);
  cloud.height = 1;
  cloud.width  = 0;
  cloud.fields.resize(6);
  cloud.fields[0].offset = 0;
  cloud.fields[0].name = "x";
  cloud.fields[0].count = 1;
  cloud.fields[0].datatype = sensor_msgs::PointField::FLOAT32;
  cloud.fields[1].offset = 4;
  cloud.fields[1].name = "y";
  cloud.fields[1].count = 1;
  cloud.fields[1].datatype = sensor_msgs::PointField::FLOAT32;
  cloud.fields[2].offset = 8;
  cloud.fields[2].name = "z";
  cloud.fields[2].count = 1;
  cloud.fields[2].datatype = sensor_msgs::PointField::FLOAT32;
  cloud.fields[3].offset = 12;
  cloud.fields[3].name = "intensity";
  cloud.fields[3].count = 1;
  cloud.fields[3].datatype = sensor_msgs::PointField::FLOAT32;
  cloud.fields[4].offset = 16;
  cloud.fields[4].name = "tag";
  cloud.fields[4].count = 1;
  cloud.fields[4].datatype = sensor_msgs::PointField::UINT8;
  cloud.fields[5].offset = 17;
  cloud.fields[5].name = "line";
  cloud.fields[5].count = 1;
  cloud.fields[5].datatype = sensor_msgs::PointField::UINT8;

  cloud.data.resize(packet_num * kMaxPointPerEthPacket *
                    sizeof(LivoxPointXyzrtl));
  cloud.point_step = sizeof(LivoxPointXyzrtl);
  uint8_t *point_base = cloud.data.data(); /* point to the front of cloud.data ? */
  /* point_base is the final usefull data, first you need to
  create and allocate memory for point_base, also called point_dst, in this case
  the pointcloud2d's data, then use the lds.cpp defined LivoxExtendRawPointToPxyzrtl to convert from
  raw and copy into point_base */
  uint8_t data_source = lidar->data_src;
  while ((published_packet < packet_num) && !QueueIsEmpty(queue)) {
    QueueProPop(queue, &storage_packet);
    LivoxEthPacket *raw_packet =
        reinterpret_cast<LivoxEthPacket *>(storage_packet.raw_data); /* From StoragePacket to LivoxEthPacket */
    timestamp = GetStoragePacketTimestamp(&storage_packet, data_source);
    int64_t packet_gap = timestamp - last_timestamp;
    if ((packet_gap > lidar->packet_interval_max) && 
        lidar->data_is_pubulished) {
      //ROS_INFO("Lidar[%d] packet time interval is %ldns", handle, packet_gap);
      if (kSourceLvxFile != data_source) { /* by default will enter this scope */
        point_base = FillZeroPointXyzrtl(point_base, storage_packet.point_num); /* initialize the point_base to zero default */
        cloud.width += storage_packet.point_num;
        last_timestamp = last_timestamp + lidar->packet_interval;
        if (!published_packet) { 
          cloud.header.stamp = ros::Time(last_timestamp / 1000000000.0);
        }
        ++published_packet;
        continue;
      }
    }
    /** Use the first packet timestamp as pointcloud2 msg timestamp */
    if (!published_packet) { 
      cloud.header.stamp = ros::Time(timestamp / 1000000000.0);
    }
    cloud.width += storage_packet.point_num;

    if (kSourceLvxFile != data_source) { /* by default will enter this scope */
      PointConvertHandler pf_point_convert = /*PointConvertHandler is a pointer to function, defined in lds.h */
          GetConvertHandler(lidar->raw_data_type); /* defined in lds.cpp, by default leads to LivoxExtendRawPointToPxyzrtl in lds.cpp */
      ROS_INFO_ONCE("raw_data_type %u", lidar->raw_data_type);
      /* extrinsic_paramter is set in LdsLidar::GetLidarExtrinsicParameterCb in lds_lidar.cpp*/
      if (pf_point_convert) {
        ROS_WARN_ONCE("Wei is here! Extrinsic %s\n", lidar->extrinsic_parameter.enable?"Yes":"No");
        point_base = pf_point_convert(
            point_base, raw_packet, lidar->extrinsic_parameter); /* convert LivoxEthPacket to point_base in the cloud.data, uint_8 * */
      } else {
        /** Skip the packet */
        ROS_INFO("Lidar[%d] unkown packet type[%d]", handle,
                 raw_packet->data_type);
        break;
      }
    } else {
      point_base = LivoxPointToPxyzrtl(
          point_base, raw_packet, lidar->extrinsic_parameter);
    }

    QueuePopUpdate(queue);
    last_timestamp = timestamp;
    ++published_packet;
  }

  cloud.row_step = cloud.width * cloud.point_step;
  cloud.is_bigendian = false;
  cloud.is_dense = true;
  cloud.data.resize(cloud.row_step); /** Adjust to the real size */

  ros::Publisher *p_publisher = Lddc::GetCurrentPublisher(handle);
  if (kOutputToRos == output_type_) {
    p_publisher->publish(cloud);
  } else {
    if (bag_) {
      bag_->write(p_publisher->getTopic(), ros::Time(timestamp / 1000000000.0),
                  cloud);
    }
  }

  if (!lidar->data_is_pubulished) {
    lidar->data_is_pubulished = true;
  }

  return published_packet;
}

/* for pcl::pxyzi */
uint32_t Lddc::PublishPointcloudData(LidarDataQueue *queue, uint32_t packet_num,
                                     uint8_t handle) {
  uint64_t timestamp = 0;
  uint64_t last_timestamp = 0;
  uint32_t published_packet = 0;

  StoragePacket storage_packet;
  LidarDevice *lidar  = &lds_->lidars_[handle];
  if (GetPublishStartTime(lidar, queue, &last_timestamp, &storage_packet)) {
    /* the remaning packets in queue maybe not enough after skip */
    return 0;
  }

  /* init point cloud data struct */
  PointCloud::Ptr cloud(new PointCloud);
  cloud->header.frame_id.assign(frame_id_);
  /* cloud->header.stamp = ros::Time::now(); */
  cloud->height = 1;
  cloud->width  = 0;

  uint8_t data_source = lidar->data_src;
  while ((published_packet < packet_num) && !QueueIsEmpty(queue)) {
    QueueProPop(queue, &storage_packet);
    LivoxEthPacket *raw_packet =
        reinterpret_cast<LivoxEthPacket *>(storage_packet.raw_data);
    timestamp = GetStoragePacketTimestamp(&storage_packet, data_source);
    int64_t packet_gap = timestamp - last_timestamp;
    if ((packet_gap > lidar->packet_interval_max) &&
        lidar->data_is_pubulished) {
      //ROS_INFO("Lidar[%d] packet time interval is %ldns", handle, packet_gap);
      pcl::PointXYZI point = {0}; /* fill zero points */
      for (uint32_t i = 0; i < storage_packet.point_num; i++) {
        cloud->points.push_back(point);
      }
      last_timestamp = last_timestamp + lidar->packet_interval;
      if (!published_packet) { 
          cloud->header.stamp = last_timestamp / 1000.0; // to pcl ros time stamp
      }
      ++published_packet;
      continue;
    }
    if (!published_packet) {
      cloud->header.stamp = timestamp / 1000.0; // to pcl ros time stamp
    }
    cloud->width += storage_packet.point_num;

    uint8_t point_buf[2048];
    if (kSourceLvxFile != data_source) {
      PointConvertHandler pf_point_convert =
          GetConvertHandler(lidar->raw_data_type);
      if (pf_point_convert) {
        pf_point_convert(point_buf, raw_packet,
                         lidar->extrinsic_parameter);
      } else {
        /* Skip the packet */
        ROS_INFO("Lidar[%d] unkown packet type[%d]", handle,
                 raw_packet->data_type);
        break;
      }
    } else {
      LivoxPointToPxyzrtl(point_buf, raw_packet,
                          lidar->extrinsic_parameter);
    }

    LivoxPointXyzrtl *dst_point = (LivoxPointXyzrtl *)point_buf;
    for (uint32_t i = 0; i < storage_packet.point_num; i++) {
      pcl::PointXYZI point;
      point.x = dst_point->x;
      point.y = dst_point->y;
      point.z = dst_point->z;
      point.intensity = dst_point->reflectivity;
      ++dst_point;
      cloud->points.push_back(point);
    }

    QueuePopUpdate(queue);
    last_timestamp = timestamp;
    ++published_packet;
  }

  ros::Publisher *p_publisher = Lddc::GetCurrentPublisher(handle);
  if (kOutputToRos == output_type_) {
    p_publisher->publish(cloud);
  } else {
    if (bag_) {
      bag_->write(p_publisher->getTopic(), ros::Time(timestamp / 1000000000.0),
                  cloud);
    }
  }

  if (!lidar->data_is_pubulished) {
    lidar->data_is_pubulished = true;
  }

  return published_packet;
}

uint32_t Lddc::PublishCustomPointcloud(LidarDataQueue *queue,
                                       uint32_t packet_num, uint8_t handle) {
  static uint32_t msg_seq = 0;
  uint64_t timestamp = 0;
  uint64_t last_timestamp = 0;
  uint32_t published_packet = 0;
  uint32_t packet_offset_time = 0; // ns

  livox_ros_driver::CustomMsg livox_msg;

  //livox_msg.header.frame_id = "livox_frame";
  livox_msg.header.frame_id.assign(frame_id_);
  
  livox_msg.header.seq = msg_seq;
  ++msg_seq;
  // livox_msg.header.stamp = ros::Time::now();
  livox_msg.timebase = 0;
  livox_msg.point_num = 0;
  livox_msg.lidar_id = handle;

  LidarDevice *lidar  = &lds_->lidars_[handle];
  uint8_t data_source = lds_->lidars_[handle].data_src;
  StoragePacket storage_packet;
  while (published_packet < packet_num) {
    QueueProPop(queue, &storage_packet);
    LivoxEthPacket *raw_packet =
        reinterpret_cast<LivoxEthPacket *>(storage_packet.raw_data);
    uint32_t point_interval = GetPointInterval(lidar->raw_data_type);
    uint32_t dual_point = 0;
    if ((raw_packet->data_type == kDualExtendCartesian) ||
        (raw_packet->data_type == kDualExtendSpherical)) {
      dual_point = 1;
    }

    timestamp = GetStoragePacketTimestamp(&storage_packet, data_source);
    if (((timestamp - last_timestamp) > kDeviceDisconnectThreshold) &&
        published_packet && lidar->data_is_pubulished) {
      ROS_INFO("Lidar[%d] packet loss", handle);
      break;
    }
    if (!published_packet) {
      livox_msg.timebase = timestamp; // to us
      packet_offset_time = 0;         // first packet
      livox_msg.header.stamp =
          ros::Time(timestamp / 1000000000.0); // to ros time stamp
      // ROS_DEBUG("[%d]:%ld %d", handle, livox_msg.timebase, point_interval);
    } else {
      packet_offset_time = (uint32_t)(timestamp - livox_msg.timebase);
    }
    livox_msg.point_num += storage_packet.point_num;

    uint8_t point_buf[2048];
    if (kSourceLvxFile != data_source) {
      PointConvertHandler pf_point_convert =
          GetConvertHandler(lidar->raw_data_type);
      if (pf_point_convert) {
        pf_point_convert(point_buf, raw_packet,
                         lidar->extrinsic_parameter);
      } else {
        /* Skip the packet */
        ROS_INFO("Lidar[%d] unkown packet type[%d]", handle,
                 lidar->raw_data_type);
        break;
      }
    } else {
      LivoxPointToPxyzrtl(point_buf, raw_packet,
                          lidar->extrinsic_parameter);
    }

    LivoxPointXyzrtl *dst_point = (LivoxPointXyzrtl *)point_buf;
    for (uint32_t i = 0; i < storage_packet.point_num; i++) {
      livox_ros_driver::CustomPoint point;
      if (!dual_point) { /** dual return mode */
        point.offset_time = packet_offset_time + i * point_interval;
      } else {
        point.offset_time = packet_offset_time + (i / 2) * point_interval;
      }
      point.x = dst_point->x;
      point.y = dst_point->y;
      point.z = dst_point->z;
      point.reflectivity = dst_point->reflectivity;
      point.tag = dst_point->tag;
      point.line = dst_point->line;
      ++dst_point;
      livox_msg.points.push_back(point);
    }

    QueuePopUpdate(queue);
    last_timestamp = timestamp;
    ++published_packet;
  }

  ros::Publisher *p_publisher = Lddc::GetCurrentPublisher(handle);
  if (kOutputToRos == output_type_) {
    p_publisher->publish(livox_msg);
  } else {
    if (bag_) {
      bag_->write(p_publisher->getTopic(), ros::Time(timestamp / 1000000000.0),
                  livox_msg);
    }
  }

  if (!lidar->data_is_pubulished) {
    lidar->data_is_pubulished = true;
  }

  return published_packet;
}

uint32_t Lddc::PublishImuData(LidarDataQueue *queue, uint32_t packet_num,
                              uint8_t handle) {
  uint64_t timestamp = 0;
  uint32_t published_packet = 0;

  sensor_msgs::Imu imu_data;
  imu_data.header.frame_id = "livox_frame";

  uint8_t data_source = lds_->lidars_[handle].data_src;
  StoragePacket storage_packet;
  QueueProPop(queue, &storage_packet);
  LivoxEthPacket *raw_packet =
      reinterpret_cast<LivoxEthPacket *>(storage_packet.raw_data);
  timestamp = GetStoragePacketTimestamp(&storage_packet, data_source);
  if (timestamp >= 0) {
    imu_data.header.stamp =
        ros::Time(timestamp / 1000000000.0); // to ros time stamp
  }

  uint8_t point_buf[2048];
  LivoxImuDataProcess(point_buf, raw_packet);

  LivoxImuPoint *imu = (LivoxImuPoint *)point_buf;
  imu_data.angular_velocity.x = imu->gyro_x;
  imu_data.angular_velocity.y = imu->gyro_y;
  imu_data.angular_velocity.z = imu->gyro_z;
  imu_data.linear_acceleration.x = imu->acc_x;
  imu_data.linear_acceleration.y = imu->acc_y;
  imu_data.linear_acceleration.z = imu->acc_z;

  QueuePopUpdate(queue);
  ++published_packet;

  ros::Publisher *p_publisher = Lddc::GetCurrentImuPublisher(handle);
  if (kOutputToRos == output_type_) {
    p_publisher->publish(imu_data);
  } else {
    if (bag_) {
      bag_->write(p_publisher->getTopic(), ros::Time(timestamp / 1000000000.0),
                  imu_data);
    }
  }

  return published_packet;
}

int Lddc::RegisterLds(Lds *lds) {
  if (lds_ == nullptr) {
    lds_ = lds;
    return 0;
  } else {
    return -1;
  }
}

/* @Wei key lidar pointcloud function called in DistributeLidarData */
void Lddc::PollingLidarPointCloudData(uint8_t handle, LidarDevice *lidar) {
  LidarDataQueue *p_queue = &lidar->data; /* fetch the lidar data queue generated in lds_lidar */
  if (p_queue == nullptr) {
    return;
  }

  while (!QueueIsEmpty(p_queue)) {
    uint32_t used_size = QueueUsedSize(p_queue);
    uint32_t onetime_publish_packets =
        GetPacketNumPerSec(lidar->raw_data_type) / publish_frq_; /* number of packets published within each intervel */
    if (used_size < onetime_publish_packets) {
      break;
    }

    if (kPointCloud2Msg == transfer_format_) { /* by default the PointCloud2.msg */
      PublishPointcloud2(p_queue, onetime_publish_packets, handle); /* finally publish from queue */
    } else if (kLivoxCustomMsg == transfer_format_) {
      PublishCustomPointcloud(p_queue, onetime_publish_packets, handle);
    } else if (kPclPxyziMsg == transfer_format_) {
      PublishPointcloudData(p_queue, onetime_publish_packets, handle);
    }
  }
}

/* @Wei Imu function called in DistributeLidarData */
void Lddc::PollingLidarImuData(uint8_t handle, LidarDevice *lidar) {
  LidarDataQueue *p_queue = &lidar->imu_data;
  if (p_queue == nullptr) {
    return;
  }

  while (!QueueIsEmpty(p_queue)) {
    PublishImuData(p_queue, 1, handle);
  }
}

/* @Wei Key key function in main loop, level 1 */
void Lddc::DistributeLidarData(void) {
  if (lds_ == nullptr) {
    return;
  }

  for (uint32_t i = 0; i < lds_->lidar_count_; i++) {
    uint32_t lidar_id = i;
    LidarDevice *lidar = &lds_->lidars_[lidar_id];
    LidarDataQueue *p_queue = &lidar->data; /* lidar data queue converted from eth_packed in lds_lidar.cpp */
    if ((kConnectStateSampling != lidar->connect_state) ||
        (p_queue == nullptr)) {
      continue;
    }

    PollingLidarPointCloudData(lidar_id, lidar);
    PollingLidarImuData(lidar_id, lidar);
  }

  if (lds_->IsRequestExit()) {
    PrepareExit();
  }
}

ros::Publisher *Lddc::GetCurrentPublisher(uint8_t handle) {
  ros::Publisher **pub = nullptr;
  uint32_t queue_size = kMinEthPacketQueueSize;

  if (use_multi_topic_) {
    pub = &private_pub_[handle];
  } else {
    pub = &global_pub_;
    queue_size = queue_size * 8;
  }

  if (*pub == nullptr) {
    char name_str[48];
    memset(name_str, 0, sizeof(name_str));
    if (use_multi_topic_) {
      snprintf(name_str, sizeof(name_str), "livox/lidar_%s",
               lds_->lidars_[handle].info.broadcast_code);
      ROS_INFO("Support multi topics.");
    } else {
      ROS_INFO("Support only one topic.");
      snprintf(name_str, sizeof(name_str), "livox/lidar");
    }

    *pub = new ros::Publisher;
    if (kPointCloud2Msg == transfer_format_) {
      **pub =
          cur_node_->advertise<sensor_msgs::PointCloud2>(name_str, queue_size);
      ROS_INFO(
          "%s publish use PointCloud2 format, set ROS publisher queue size %d",
          name_str, queue_size);
    } else if (kLivoxCustomMsg == transfer_format_) {
      **pub = cur_node_->advertise<livox_ros_driver::CustomMsg>(name_str,
                                                                queue_size);
      ROS_INFO(
          "%s publish use livox custom format, set ROS publisher queue size %d",
          name_str, queue_size);
    } else if (kPclPxyziMsg == transfer_format_) {
      **pub = cur_node_->advertise<PointCloud>(name_str, queue_size);
      ROS_INFO("%s publish use pcl PointXYZI format, set ROS publisher queue "
               "size %d",
               name_str, queue_size);
    }
  }

  return *pub;
}

ros::Publisher *Lddc::GetCurrentImuPublisher(uint8_t handle) {
  ros::Publisher **pub = nullptr;
  uint32_t queue_size = kMinEthPacketQueueSize;

  if (use_multi_topic_) {
    pub = &private_imu_pub_[handle];
  } else {
    pub = &global_imu_pub_;
    queue_size = queue_size * 4;
  }

  if (*pub == nullptr) {
    char name_str[48];
    memset(name_str, 0, sizeof(name_str));
    if (use_multi_topic_) {
      ROS_INFO("Support multi topics.");
      snprintf(name_str, sizeof(name_str), "livox/imu_%s",
               lds_->lidars_[handle].info.broadcast_code);
    } else {
      ROS_INFO("Support only one topic.");
      snprintf(name_str, sizeof(name_str), "livox/imu");
    }

    *pub = new ros::Publisher;
    **pub = cur_node_->advertise<sensor_msgs::Imu>(name_str, queue_size);
    ROS_INFO("%s publish imu data, set ROS publisher queue size %d", name_str,
             queue_size);
  }

  return *pub;
}

void Lddc::CreateBagFile(const std::string &file_name) {
  if (!bag_) {
    bag_ = new rosbag::Bag;
    bag_->open(file_name, rosbag::bagmode::Write);
    ROS_INFO("Create bag file :%s!", file_name.c_str());
  }
}

void Lddc::PrepareExit(void) {
  if (bag_) {
    bag_->close();

    ROS_INFO("Press [Ctrl+C] to exit!\n");
    bag_ = nullptr;
  }
  if (lds_) {
    lds_->PrepareExit();
    lds_ = nullptr;
  }
}

} // namespace livox_ros
