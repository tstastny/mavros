/**
 * @brief Odometry plugin
 * @file odom.cpp
 * @author James Goppert <james.goppert@gmail.com>
 * @author Nuno Marques <n.marques21@hotmail.com>
 *
 * @addtogroup plugin
 * @{
 */
/*
 * Copyright 2017 James Goppert
 * Copyright 2017,2018 Nuno Marques
 *
 * This file is part of the mavros package and subject to the license terms
 * in the top-level LICENSE file of the mavros repository.
 * https://github.com/mavlink/mavros/tree/master/LICENSE.md
 */

#include <mavros/mavros_plugin.h>
#include <tf2_eigen/tf2_eigen.h>
#include <boost/algorithm/string.hpp>

#include <nav_msgs/Odometry.h>

namespace mavros {
namespace extra_plugins {
using mavlink::common::MAV_FRAME;

/**
 * @brief Odometry plugin
 *
 * Sends odometry data to the FCU position and attitude estimators.
 * @see odom_cb()
 */
class OdometryPlugin : public plugin::PluginBase {
public:
	OdometryPlugin() : PluginBase(),
		odom_nh("~odometry"),
		local_frame("vision_ned"),
		body_frame_orientation("frd")
	{ }

	void initialize(UAS &uas_)
	{
		PluginBase::initialize(uas_);

		// frame tf params
		odom_nh.param<std::string>("frame_tf/local_frame", local_frame, "vision_ned");
		odom_nh.param<std::string>("frame_tf/body_frame_orientation", body_frame_orientation, "frd");

		boost::algorithm::to_lower(local_frame);
		boost::algorithm::to_lower(body_frame_orientation);

		// subscribers
		odom_sub = odom_nh.subscribe("odom", 10, &OdometryPlugin::odom_cb, this);

		//! Map from param string to local MAV_FRAME
		const std::unordered_map<std::string, MAV_FRAME> lf_map {
			{ "vision_enu", MAV_FRAME::VISION_ENU },
			{ "vision_ned", MAV_FRAME::VISION_NED },
			{ "mocap_enu", MAV_FRAME::MOCAP_ENU },
			{ "mocap_ned", MAV_FRAME::MOCAP_NED }
		};

		//! Map from param string to body MAV_FRAME
		const std::unordered_map<std::string, MAV_FRAME> bf_map {
			{ "ned", MAV_FRAME::BODY_NED },
			{ "frd", MAV_FRAME::BODY_FRD },
			{ "flu", MAV_FRAME::BODY_FLU }
		};
		// Determine frame_id naming - considering the ROS msg frame_id
		// as "odom" by default
		auto lf_it = lf_map.find(local_frame);
		if (lf_it != lf_map.end()) {
			lf_id = lf_it->second;
			local_frame_orientation = "local_origin_" + local_frame.substr(local_frame.length() - 3);
		}
		else
			ROS_FATAL_NAMED("odom", "ODOM: invalid local frame \"%s\"", local_frame.c_str());

		// Determine child_frame_id naming
		auto bf_it = bf_map.find(body_frame_orientation);
		if (bf_it == bf_map.end()) {
			bf_id = bf_it->second;
			body_frame_orientation = "fcu_" + body_frame_orientation;
		}
		else
			ROS_FATAL_NAMED("odom", "ODOM: invalid body frame orientation \"%s\"", body_frame_orientation.c_str());
	}

	Subscriptions get_subscriptions()
	{
		return { /* Rx disabled */ };
	}

private:
	ros::NodeHandle odom_nh;
	ros::Subscriber odom_sub;

	std::string local_frame;		//!< orientation and source of the local frame (pose)
	std::string body_frame_orientation;	//!< orientation of the body frame (velocity)
	std::string local_frame_orientation;

	MAV_FRAME lf_id;			//!< local frame (pose) ID
	MAV_FRAME bf_id;			//!< body frame (pose) ID

	/**
	 * @brief Sends odometry data msgs to the FCU.
	 *
	 * Message specification: https://mavlink.io/en/messages/common.html#ODOMETRY
	 * @param req	received Odometry msg
	 */
	void odom_cb(const nav_msgs::Odometry::ConstPtr &odom)
	{
		/** Lookup transforms
		 *  @todo Implement in a more general fashion in the API IOT apply frame transforms
		 *  This should also run in parallel on a thread
		 */
		Eigen::Affine3d tf_child2local;
		Eigen::Affine3d tf_child2body;
		Eigen::Affine3d tf_parent2local;

		try {
			// transform lookup WRT local frame
			tf_child2local = tf2::transformToEigen(m_uas->tf2_buffer.lookupTransform(
							odom->child_frame_id, local_frame_orientation,
							ros::Time(0)));
			tf_child2body = tf2::transformToEigen(m_uas->tf2_buffer.lookupTransform(
							odom->child_frame_id, body_frame_orientation,
							ros::Time(0)));

			// transform lookup WRT body frame
			tf_parent2local = tf2::transformToEigen(m_uas->tf2_buffer.lookupTransform(
							odom->header.frame_id, local_frame_orientation,
							ros::Time(0)));
		} catch (tf2::TransformException &ex) {
			ROS_ERROR_THROTTLE_NAMED(1, "odom", "ODOM: Ex: %s", ex.what());
			return;
		}

		/** Build 6x6 pose covariance matrix to be transformed and sent
		 */
		ftf::Covariance6d cov_pose {};	// zero initialized
		ftf::EigenMapCovariance6d cov_pose_map(cov_pose.data());

		/** Build 6x6 velocity covariance matrix to be transformed and sent
		 */
		ftf::Covariance6d cov_vel {};	// zero initialized
		ftf::EigenMapCovariance6d cov_vel_map(cov_vel.data());

		/** Apply linear transforms
		 */
		Eigen::Vector3d pos_local(tf_parent2local.linear() * ftf::to_eigen(odom->pose.pose.position));
		Eigen::Vector3d lin_vel_local(tf_child2local.linear() * ftf::to_eigen(odom->twist.twist.linear));
		Eigen::Vector3d ang_vel_body(tf_child2body.linear() * ftf::to_eigen(odom->twist.twist.angular));
		Eigen::Quaterniond q_parent2child(ftf::to_eigen(odom->pose.pose.orientation));
		Eigen::Affine3d tf_local2body = tf_parent2local * q_parent2child * tf_child2body.inverse();
		Eigen::Quaterniond q_local2body(tf_local2body.linear());

		/** Apply covariance transforms */
		/** Pose 6-D Covariance matrix
		 *  WRT local frame
		 */
		Eigen::Matrix<double, 6, 6> r_pose {};
		r_pose.setZero();	// initialize with zeros
		r_pose.block<3, 3>(0, 0) = r_pose.block<3, 3>(3, 3) = tf_parent2local.linear();

		// apply the transform
		cov_pose_map = r_pose * cov_pose_map * r_pose.transpose();

		ROS_DEBUG_STREAM_NAMED("odom", "ODOM: pose covariance matrix:" << std::endl << cov_pose_map);

		/** Velocity 6-D Covariance matrix
		 *  WRT body frame
		 */
		Eigen::Matrix<double, 6, 6> r_vel;
		r_vel.setZero();	// initialize with zeros
		r_vel.block<3, 3>(0, 0) = r_vel.block<3, 3>(3, 3) = tf_child2local.linear();

		// apply the transform
		cov_vel_map = r_vel * cov_vel_map * r_vel.transpose();

		ROS_DEBUG_STREAM_NAMED("odom", "ODOM: velocity covariance matrix:" << std::endl << cov_vel_map);

		/* -*- ODOMETRY msg parser -*- */
		mavlink::common::msg::ODOMETRY msg {};

		msg.time_usec = odom->header.stamp.toNSec() / 1e3;
		msg.frame_id = utils::enum_value(lf_id);
		msg.child_frame_id = utils::enum_value(bf_id);

		// [[[cog:
		// for a, b in (('', 'pos_local'), ('v', 'lin_vel_local')):
		//     for f in 'xyz':
		//         cog.outl("msg.{a}{f} = {b}.{f}();".format(**locals()))
		// for a, b in zip("xyz", ('rollspeed', 'pitchspeed', 'yawspeed')):
		//     cog.outl("msg.{b} = ang_vel_body.{a}();".format(**locals()))
		// ]]]
		msg.x = pos_local.x();
		msg.y = pos_local.y();
		msg.z = pos_local.z();
		msg.vx = lin_vel_local.x();
		msg.vy = lin_vel_local.y();
		msg.vz = lin_vel_local.z();
		msg.rollspeed = ang_vel_body.x();
		msg.pitchspeed = ang_vel_body.y();
		msg.yawspeed = ang_vel_body.z();
		// [[[end]]] (checksum: 1264d181e6501ddddb85f89df16f31de)

		ftf::quaternion_to_mavlink(q_local2body, msg.q);
		ftf::covariance_urt_to_mavlink(cov_pose_map, msg.pose_covariance);
		ftf::covariance_urt_to_mavlink(cov_vel_map, msg.twist_covariance);

		// send ODOMETRY msg
		UAS_FCU(m_uas)->send_message_ignore_drop(msg);
	}
};
}	// namespace extra_plugins
}	// namespace mavros

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(mavros::extra_plugins::OdometryPlugin, mavros::plugin::PluginBase)
