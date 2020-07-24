LdsLvx push the LivoxEthPacket(defined in livox_def.h) *eth_packet to LidarDataQueue(defined in ldq.x) *p_queue
Lddc then extract the data from LidarDataQueue and convert to LivoxPointXyzrtl, need to allocated new memory for the LivoxPointXyzrtl array, in this case a sensor_msgs::PointCloud2 cloud is created to stored the new data
the GetConvertHandler(defined in lds.h) returns the conversion corresponding to data_type, for example, LivoxExtendRawPoint, please keep in mind LivoxExtendRawPointToPxyzrtl's argument is LivoxEthPacket * no the actual data array LivoxExtendRawPoint*, see lds.cpp for implementations

LdsLvx -> LivoxEthPacket* -> LidarDataQueue -> LivoxEthPacket* ->LivoxExtendRawPointToPxyzrtl -> (uint8*) LivoxPointXyzrtl *