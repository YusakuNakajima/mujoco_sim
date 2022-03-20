// Copyright (c) 2022, Hoang Giang Nguyen - Institute for Artificial Intelligence, University Bremen

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "mj_sim.h"
#include "mujoco_msgs/ModelState.h"

#include <geometry_msgs/TransformStamped.h>
#include <tf2_ros/transform_broadcaster.h>
#include <visualization_msgs/Marker.h>

class MjRos
{
public:
    MjRos() = default;

    ~MjRos();

public:
    /**
     * @brief Initialize publishers, subscribers and MjRos members from rosparam
     *
     */
    void init();

    /**
     * @brief Update publishers
     *
     */
    void update(double frequency);

private:
    /**
     * @brief Create new data file of new object from ROS
     *
     * @param msg New object parameters
     */
    void object_gen_callback(const mujoco_msgs::ModelState &msg);

    /**
     * @brief Publish markers for all objects
     *
     * @param body_idx Body index of the object
     * @param object_name Name of the object (object_name = mj_id2name(m, mjtObj::mjOBJ_BODY, body_idx))
     */
    void publish_markers(int body_idx, std::string object_name);

    /**
     * @brief Publish tf of all objects
     *
     * @param body_idx Body index of the object
     * @param object_name Name of the object (object_name = mj_id2name(m, mjtObj::mjOBJ_BODY, body_idx))
     */
    void publish_tf(int body_idx, std::string object_name);

public:
    /**
     * @brief Start time of ROS
     *
     */
    static ros::Time ros_start;

private:
    ros::NodeHandle n;

    ros::Subscriber object_gen_sub;

    visualization_msgs::Marker marker;

    ros::Publisher vis_pub;

    geometry_msgs::TransformStamped transform;

    tf2_ros::TransformBroadcaster br;
};