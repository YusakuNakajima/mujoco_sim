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

#include "mj_ros.h"

#include <algorithm>
#include <condition_variable>
#include <ros/package.h>
#include <tinyxml2.h>

using namespace std::chrono_literals;

ros::Time MjRos::ros_start;

std::string root_name;

bool pub_object_marker;
bool pub_object_tf;
bool pub_object_states;

visualization_msgs::Marker marker;
geometry_msgs::TransformStamped transform;
nav_msgs::Odometry base_pose;
mujoco_msgs::ObjectStateArray object_states;

std::condition_variable condition;

int spawn_nr = 0;
std::mutex spawn_mtx;
std::vector<mujoco_msgs::ObjectStatus> objects_to_spawn;
bool spawn_success;

int destroy_nr = 0;
std::mutex destroy_mtx;
std::vector<std::string> object_names_to_destroy;
bool destroy_success;

MjRos::~MjRos()
{
}

void MjRos::init()
{
    n = ros::NodeHandle();

    if (!ros::param::get("~pub_object_marker", pub_object_marker))
    {
        pub_object_marker = true;
    }
    if (!ros::param::get("~pub_object_tf", pub_object_tf))
    {
        pub_object_tf = true;
    }
    if (!ros::param::get("~pub_object_states", pub_object_states))
    {
        pub_object_states = true;
    }
    if (!ros::param::get("~root_frame_id", root_frame_id))
    {
        root_frame_id = "map";
    }

    ros_start = ros::Time::now();

    int joint_id;
    std::string link_name;
    for (const std::string joint_name : MjSim::joint_names)
    {
        joint_id = mj_name2id(m, mjtObj::mjOBJ_JOINT, joint_name.c_str());
        link_name = mj_id2name(m, mjtObj::mjOBJ_BODY, m->jnt_bodyid[joint_id]);
        MjSim::link_names.push_back(link_name);
    }

    if (use_odom_joints)
    {
        cmd_vel_sub = n.subscribe("cmd_vel", 10, &MjRos::cmd_vel_callback, this);
    }

    reset_robot_server = n.advertiseService("reset", &MjRos::reset_robot_service, this);
    ROS_INFO("Started [%s] service.", reset_robot_server.getService().c_str());

    spawn_objects_server = n.advertiseService("/mujoco/spawn_objects", &MjRos::spawn_objects_service, this);
    ROS_INFO("Started [%s] service.", spawn_objects_server.getService().c_str());

    destroy_objects_server = n.advertiseService("/mujoco/destroy_objects", &MjRos::destroy_objects_service, this);
    ROS_INFO("Started [%s] service.", destroy_objects_server.getService().c_str());

    urdf::Model urdf_model;
    if (init_urdf(urdf_model, n)) // this looks so retared...
    {
        root_name = urdf_model.getRoot()->name;
        for (const std::pair<std::string, urdf::JointSharedPtr> &joint : urdf_model.joints_)
        {
            if (joint.second->mimic != nullptr)
            {
                MimicJoint mimic_joint;
                mimic_joint.from_joint = joint.second->mimic->joint_name;
                mimic_joint.multiplier = joint.second->mimic->multiplier;
                mimic_joint.offset = joint.second->mimic->offset;
                MjSim::mimic_joints[joint.first] = mimic_joint;
            }
        }
    }
    if (!ros::param::get("~joint_inits", joint_inits))
    {
        ROS_WARN("joint_inits not found, will set to default value (0)");
    }

    marker_pub = n.advertise<visualization_msgs::Marker>("/mujoco/visualization_marker", 0);
    base_pose_pub = n.advertise<nav_msgs::Odometry>(root_name, 0);
    object_states_pub = n.advertise<mujoco_msgs::ObjectStateArray>("/mujoco/object_states", 0);

    reset_robot();
}

void MjRos::reset_robot()
{
    for (const std::string &joint_name : MjSim::joint_names)
    {
        const int joint_id = mj_name2id(m, mjtObj::mjOBJ_JOINT, joint_name.c_str());
        if (joint_id != -1)
        {
            if (joint_inits.count(joint_name) != 0)
            {
                d->qpos[joint_id] = joint_inits[joint_name];
            }
            else
            {
                d->qpos[joint_id] = 0.f;
            }
        }
    }
    if (use_odom_joints)
    {
        const std::vector<std::string> odom_joints = {"odom_x_joint", "odom_y_joint", "odom_z_joint"};
        for (const std::string &odom_joint : odom_joints)
        {
            MjSim::odom_joints[odom_joint].second = 0.f;
            const int joint_id = mj_name2id(m, mjtObj::mjOBJ_JOINT, odom_joint.c_str());
            if (joint_id != -1)
            {
                d->qpos[joint_id] = 0.f;
            }
        }
    }
    mj_forward(m, d);
}

bool MjRos::reset_robot_service(std_srvs::TriggerRequest &req, std_srvs::TriggerResponse &res)
{
    mtx.lock();
    reset_robot();
    mtx.unlock();
    ros::Duration(100 * m->opt.timestep).sleep();
    float error_sum = 0.f;
    for (const std::string &joint_name : MjSim::joint_names)
    {
        const int joint_id = mj_name2id(m, mjtObj::mjOBJ_JOINT, joint_name.c_str());
        if (joint_id != -1)
        {
            if (joint_inits.count(joint_name) != 0)
            {
                error_sum += mju_abs(d->qpos[joint_id] - joint_inits[joint_name]);
            }
            else
            {
                error_sum += mju_abs(d->qpos[joint_id]);
            }
        }
    }
    if (error_sum < MjSim::joint_names.size() * 1E-2)
    {
        res.success = true;
        res.message = "Reset successfully! (error_sum = " + std::to_string(error_sum) + ")";
    }
    else
    {
        res.success = false;
        res.message = "Failed to reset (error_sum = " + std::to_string(error_sum) + "). Did you stop the controlllers?";
    }
    return true;
}

bool MjRos::spawn_objects_service(mujoco_msgs::SpawnObjectRequest &req, mujoco_msgs::SpawnObjectResponse &res)
{
    std::vector<std::string> names;
    for (const mujoco_msgs::ObjectStatus &object : req.objects)
    {
        if (std::find(objects_to_spawn.begin(), objects_to_spawn.end(), object) == objects_to_spawn.end() &&
            mj_name2id(m, mjtObj::mjOBJ_BODY, object.info.name.c_str()) == -1)
        {
            objects_to_spawn.push_back(object);
            names.push_back(object.info.name);
        }
    }
    if (objects_to_spawn.empty())
    {
        res.names = std::vector<std::string>();
        return true;
    }

    std::unique_lock<std::mutex> lk(spawn_mtx);
    spawn_success = false;
    if (condition.wait_until(lk, std::chrono::system_clock::now() + 100ms, [&]
                             { return spawn_success; }))
    {
        res.names = names;
    }
    else
    {
        res.names = std::vector<std::string>();
    }
    return true;
}

void MjRos::spawn_objects(const std::vector<mujoco_msgs::ObjectStatus> objects)
{
    // Create add.xml
    tinyxml2::XMLDocument object_xml_doc;
    tinyxml2::XMLNode *root = object_xml_doc.NewElement("mujoco");
    object_xml_doc.LinkEndChild(root);

    tinyxml2::XMLElement *worldbody_element = object_xml_doc.NewElement("worldbody");
    root->LinkEndChild(worldbody_element);

    for (const mujoco_msgs::ObjectStatus &object : objects)
    {
        if (mj_name2id(m, mjtObj::mjOBJ_BODY, object.info.name.c_str()) != -1)
        {
            ROS_WARN("Object [%s] already exists, ignore...", object.info.name.c_str());
            continue;
        }

        tinyxml2::XMLElement *body_element = object_xml_doc.NewElement("body");
        if (object.info.movable)
        {
            tinyxml2::XMLElement *joint_element = object_xml_doc.NewElement("freejoint");
            body_element->LinkEndChild(joint_element);
        }
        else
        {
            body_element->SetAttribute("mocap", "true");
        }

        tinyxml2::XMLElement *geom_element = object_xml_doc.NewElement("geom");
        tinyxml2::XMLElement *inertial_element = object_xml_doc.NewElement("inertial");

        boost::filesystem::path object_mesh_path = object.info.mesh;
        switch (object.info.type)
        {
        case mujoco_msgs::ObjectInfo::CUBE:
            geom_element->SetAttribute("type", "box");
            geom_element->SetAttribute("size",
                                       (std::to_string(object.info.size.x) + " " +
                                        std::to_string(object.info.size.y) + " " +
                                        std::to_string(object.info.size.z))
                                           .c_str());
            break;

        case mujoco_msgs::ObjectInfo::SPHERE:
            geom_element->SetAttribute("type", "sphere");
            geom_element->SetAttribute("size", object.info.size.x);
            break;

        case mujoco_msgs::ObjectInfo::CYLINDER:
            geom_element->SetAttribute("type", "cylinder");
            geom_element->SetAttribute("size",
                                       (std::to_string(object.info.size.x) + " " +
                                        std::to_string(object.info.size.z))
                                           .c_str());
            break;

        case mujoco_msgs::ObjectInfo::MESH:
            if (!object_mesh_path.has_extension())
            {
                if (boost::filesystem::exists(model_path.parent_path() / (object_mesh_path.string() + ".xml")))
                {
                    object_mesh_path.replace_extension(".xml");
                }
                else if (boost::filesystem::exists(model_path.parent_path() / (model_path.stem().string() + "/meshes/" + object_mesh_path.string() + ".stl")))
                {
                    object_mesh_path.replace_extension(".stl");
                }
            }

            if (object_mesh_path.extension().compare(".xml") == 0)
            {
                object_mesh_path = model_path.parent_path() / object_mesh_path;
                tinyxml2::XMLDocument mesh_xml_doc;

                if (mesh_xml_doc.LoadFile(object_mesh_path.c_str()) != tinyxml2::XML_SUCCESS)
                {
                    mju_warning_s("Failed to load file \"%s\"\n", object_mesh_path.c_str());
                    continue;
                }
                for (tinyxml2::XMLNode *node = mesh_xml_doc.FirstChild()->FirstChild();
                     node != nullptr;
                     node = node->NextSibling())
                {
                    tinyxml2::XMLNode *copy = node->DeepClone(&object_xml_doc);
                    // Don't copy asset, it should be included in config
                    if (strcmp(copy->Value(), "asset") == 0)
                    {
                        continue;
                    }

                    if (strcmp(copy->Value(), "worldbody") == 0)
                    {
                        for (tinyxml2::XMLElement *element = copy->ToElement()->FirstChildElement();
                             element != nullptr;
                             element = element->NextSiblingElement())
                        {
                            if (strcmp(element->Value(), "body") == 0)
                            {
                                element->SetAttribute("name", object.info.name.c_str());
                                element->SetAttribute("pos", (std::to_string(object.pose.position.x) + " " +
                                                              std::to_string(object.pose.position.y) + " " +
                                                              std::to_string(object.pose.position.z))
                                                                 .c_str());
                                element->SetAttribute("quat",
                                                      (std::to_string(object.pose.orientation.w) + " " +
                                                       std::to_string(object.pose.orientation.x) + " " +
                                                       std::to_string(object.pose.orientation.y) + " " +
                                                       std::to_string(object.pose.orientation.z))
                                                          .c_str());
                                break;
                            }
                        }
                    }
                    root->InsertEndChild(copy);
                }
                continue;
            }
            if (object_mesh_path.extension().compare(".stl") == 0)
            {
                geom_element->SetAttribute("type", "mesh");
                geom_element->SetAttribute("size",
                                           (std::to_string(object.info.size.x) + " " +
                                            std::to_string(object.info.size.y) + " " +
                                            std::to_string(object.info.size.z))
                                               .c_str());
                geom_element->SetAttribute("mesh", object_mesh_path.stem().c_str());
            }

        default:
            break;
        }

        body_element->SetAttribute("name", object.info.name.c_str());

        body_element->SetAttribute("pos",
                                   (std::to_string(object.pose.position.x) + " " +
                                    std::to_string(object.pose.position.y) + " " +
                                    std::to_string(object.pose.position.z))
                                       .c_str());

        body_element->SetAttribute("quat",
                                   (std::to_string(object.pose.orientation.w) + " " +
                                    std::to_string(object.pose.orientation.x) + " " +
                                    std::to_string(object.pose.orientation.y) + " " +
                                    std::to_string(object.pose.orientation.z))
                                       .c_str());

        if (object.info.inertial.m != 0)
        {
            inertial_element->SetAttribute("pos", (std::to_string(object.info.inertial.com.x) + " " +
                                                   std::to_string(object.info.inertial.com.y) + " " +
                                                   std::to_string(object.info.inertial.com.z))
                                                      .c_str());

            inertial_element->SetAttribute("mass", object.info.inertial.m);

            if (object.info.inertial.ixx != 0 ||
                object.info.inertial.ixy != 0 ||
                object.info.inertial.ixz != 0 ||
                object.info.inertial.iyy != 0 ||
                object.info.inertial.iyz != 0 ||
                object.info.inertial.izz != 0)
            {
                inertial_element->SetAttribute("fullinertia", (std::to_string(object.info.inertial.ixx) + " " +
                                                               std::to_string(object.info.inertial.iyy) + " " +
                                                               std::to_string(object.info.inertial.izz) + " " +
                                                               std::to_string(object.info.inertial.ixy) + " " +
                                                               std::to_string(object.info.inertial.ixz) + " " +
                                                               std::to_string(object.info.inertial.iyz))
                                                                  .c_str());
            }

            body_element->LinkEndChild(inertial_element);
        }

        geom_element->SetAttribute("rgba",
                                   (std::to_string(object.info.rgba.r) + " " +
                                    std::to_string(object.info.rgba.g) + " " +
                                    std::to_string(object.info.rgba.b) + " " +
                                    std::to_string(object.info.rgba.a))
                                       .c_str());

        body_element->LinkEndChild(geom_element);
        worldbody_element->LinkEndChild(body_element);
    }

    if (object_xml_doc.SaveFile((tmp_model_path.parent_path() / "add.xml").c_str()) != tinyxml2::XML_SUCCESS)
    {
        mju_warning_s("Failed to save file \"%s\"\n", (tmp_model_path.parent_path() / "add.xml").c_str());
        spawn_success = false;
    }
    else
    {
        spawn_success = MjSim::add_data();
        mtx.lock();
        for (const mujoco_msgs::ObjectStatus &object : objects)
        {
            const char *name = object.info.name.c_str();
            ROS_INFO("[Spawn #%d] Try to spawn body %s", spawn_nr, name);
            int body_id = mj_name2id(m, mjtObj::mjOBJ_BODY, name);
            if (body_id != -1)
            {
                if (m->body_dofnum[body_id] != 6)
                {
                    continue;
                }

                int dof_adr = m->jnt_dofadr[m->body_jntadr[body_id]];
                d->qvel[dof_adr] = object.velocity.linear.x;
                d->qvel[dof_adr + 1] = object.velocity.linear.y;
                d->qvel[dof_adr + 2] = object.velocity.linear.z;
                d->qvel[dof_adr + 3] = object.velocity.angular.x;
                d->qvel[dof_adr + 4] = object.velocity.angular.y;
                d->qvel[dof_adr + 5] = object.velocity.angular.z;
            }
            else
            {
                ROS_WARN("Object %s not found to spawn", name);
                spawn_success = false;
            }
        }
        spawn_nr++;
        mj_forward(m, d);
        mtx.unlock();
    }
    objects_to_spawn.erase(std::remove_if(objects_to_spawn.begin(), objects_to_spawn.end(), [objects](const mujoco_msgs::ObjectStatus &object)
                                          { return std::find(objects.begin(), objects.end(), object) != objects.end(); }),
                           objects_to_spawn.end());
}

bool MjRos::destroy_objects_service(mujoco_msgs::DestroyObjectRequest &req, mujoco_msgs::DestroyObjectResponse &res)
{
    for (const std::string &object_name : req.names)
    {
        if (std::find(object_names_to_destroy.begin(), object_names_to_destroy.end(), object_name) == object_names_to_destroy.end() &&
            mj_name2id(m, mjtObj::mjOBJ_BODY, object_name.c_str()) != -1)
        {
            object_names_to_destroy.push_back(object_name);
        }
    }

    std::vector<mujoco_msgs::ObjectState> object_states;
    if (object_names_to_destroy.empty())
    {
        res.object_states = object_states;
        return true;
    }
    else
    {
        const std::size_t objects_num = object_names_to_destroy.size();
        object_states.reserve(objects_num);

        for (int i = 0; i < objects_num; i++)
        {
            const char *name = object_names_to_destroy[i].c_str();
            ROS_INFO("[Destroy #%d] Try to destroy body %s", destroy_nr, name);
            int body_id = mj_name2id(m, mjtObj::mjOBJ_BODY, name);
            if (body_id != -1)
            {
                mujoco_msgs::ObjectState object_state;
                object_state.name = name;
                if (m->body_dofnum[body_id] != 6)
                {
                    continue;
                }

                int dof_adr = m->jnt_dofadr[m->body_jntadr[body_id]];
                object_state.velocity.linear.x = d->qvel[dof_adr];
                object_state.velocity.linear.y = d->qvel[dof_adr + 1];
                object_state.velocity.linear.z = d->qvel[dof_adr + 2];
                object_state.velocity.angular.x = d->qvel[dof_adr + 3];
                object_state.velocity.angular.y = d->qvel[dof_adr + 4];
                object_state.velocity.angular.z = d->qvel[dof_adr + 5];

                object_states.push_back(object_state);
            }
            else
            {
                ROS_WARN("Object %s not found to destroy", name);
            }
        }
        destroy_nr++;
    }

    std::unique_lock<std::mutex> lk(destroy_mtx);

    destroy_success = false;
    if (condition.wait_until(lk, std::chrono::system_clock::now() + 100ms, [&]
                             { return destroy_success; }))
    {
        res.object_states = object_states;
    }
    else
    {
        res.object_states = std::vector<mujoco_msgs::ObjectState>();
    }
    return true;
}

void MjRos::destroy_objects(const std::vector<std::string> object_names)
{
    MjSim::remove_body(object_names);
    object_names_to_destroy.erase(std::remove_if(object_names_to_destroy.begin(), object_names_to_destroy.end(), [object_names](const std::string &object_name)
                                                 { return std::find(object_names.begin(), object_names.end(), object_name) != object_names.end(); }),
                                  object_names_to_destroy.end());
}

void MjRos::cmd_vel_callback(const geometry_msgs::Twist &msg)
{
    const std::string odom_z_joint_name = MjSim::odom_joints["odom_z_joint"].first;
    const int odom_z_joint_id = mj_name2id(m, mjtObj::mjOBJ_JOINT, odom_z_joint_name.c_str());
    const mjtNum odom_z_joint_pos = d->qpos[odom_z_joint_id];

    MjSim::odom_joints["odom_x_joint"].second = msg.linear.x * mju_cos(odom_z_joint_pos) - msg.linear.y * mju_sin(odom_z_joint_pos);
    MjSim::odom_joints["odom_y_joint"].second = msg.linear.x * mju_sin(odom_z_joint_pos) + msg.linear.y * mju_cos(odom_z_joint_pos);
    MjSim::odom_joints["odom_z_joint"].second = msg.angular.z;

    base_pose.twist.twist = msg;
}

void MjRos::spawn_and_destroy_objects(const double frequency)
{
    ros::Rate loop_rate(frequency);
    while (ros::ok())
    {
        // Spawn objects
        if (objects_to_spawn.size() > 0)
        {
            std::unique_lock<std::mutex> lk(spawn_mtx);

            while (objects_to_spawn.size() > 0)
            {
                spawn_objects(objects_to_spawn);
            }

            lk.unlock();
            condition.notify_all();
        }

        // Destroy objects
        if (object_names_to_destroy.size() > 0)
        {
            std::unique_lock<std::mutex> lk(destroy_mtx);

            while (object_names_to_destroy.size() > 0)
            {
                destroy_objects(object_names_to_destroy);
            }

            lk.unlock();
            condition.notify_all();
        }
        ros::spinOnce();
        loop_rate.sleep();
    }
}

void MjRos::update(const double frequency = 60)
{
    ros::Rate loop_rate(frequency); // Update with 60 Hz

    marker.action = visualization_msgs::Marker::MODIFY;

    std_msgs::Header header;
    header.frame_id = root_frame_id;

    std::string base_name = "world";
    if (use_odom_joints)
    {
        base_name = model_path.stem().string();
    }
    base_pose.child_frame_id = root_name;

    while (ros::ok())
    {
        // Set header
        header.stamp = ros::Time::now();
        header.seq += 1;

        marker.header = header;
        transform.header = header;
        object_states.header = header;
        base_pose.header = header;

        // Publish tf and marker of objects
        std::string object_name;
        for (int body_id = 1; body_id < m->nbody; body_id++)
        {
            object_name = mj_id2name(m, mjtObj::mjOBJ_BODY, body_id);
            if (std::find(MjSim::link_names.begin(), MjSim::link_names.end(), object_name) == MjSim::link_names.end())
            {
                if (use_odom_joints && object_name == model_path.stem().string())
                {
                    continue;
                }

                if (pub_object_tf)
                {
                    set_transform(body_id, object_name);
                    br.sendTransform(transform);
                }

                if (pub_object_marker)
                {
                    publish_marker(body_id);
                }

                if (pub_object_states)
                {
                    add_object_state(body_id);
                }
            }
        }

        // Publish object states
        if (pub_object_states)
        {
            object_states_pub.publish(object_states);
        }

        // Publish tf of root
        const int root_body_id = mj_name2id(m, mjtObj::mjOBJ_BODY, base_name.c_str());
        if (root_body_id != -1)
        {
            if (pub_object_tf)
            {
                set_transform(root_body_id, root_name);
                br.sendTransform(transform);
            }

            if (use_odom_joints)
            {
                publish_base_pose(root_body_id);
            }
        }

        ros::spinOnce();
        loop_rate.sleep();
    }
}

void MjRos::publish_marker(const int body_id)
{
    const int geom_id = m->body_geomadr[body_id];
    if (geom_id != -1)
    {
        switch (m->geom_type[geom_id])
        {
        case mjtGeom::mjGEOM_BOX:
            marker.type = visualization_msgs::Marker::CUBE;
            marker.mesh_resource = "";
            marker.scale.x = m->geom_size[3 * geom_id] * 2;
            marker.scale.y = m->geom_size[3 * geom_id + 1] * 2;
            marker.scale.z = m->geom_size[3 * geom_id + 2] * 2;
            break;

        case mjtGeom::mjGEOM_SPHERE:
            marker.type = visualization_msgs::Marker::SPHERE;
            marker.mesh_resource = "";
            marker.scale.x = m->geom_size[3 * geom_id] * 2;
            marker.scale.y = m->geom_size[3 * geom_id] * 2;
            marker.scale.z = m->geom_size[3 * geom_id] * 2;
            break;

        case mjtGeom::mjGEOM_CYLINDER:
            marker.type = visualization_msgs::Marker::CYLINDER;
            marker.scale.x = m->geom_size[3 * geom_id] * 2;
            marker.scale.y = m->geom_size[3 * geom_id] * 2;
            marker.scale.z = m->geom_size[3 * geom_id + 1] * 2;
            break;

        case mjtGeom::mjGEOM_MESH:
            marker.type = visualization_msgs::Marker::MESH_RESOURCE;
            marker.mesh_resource = "package://mujoco_sim/model/tmp/" + model_path.stem().string() + "/meshes/" + mj_id2name(m, mjtObj::mjOBJ_BODY, body_id) + ".dae";
            marker.scale.x = 1;
            marker.scale.y = 1;
            marker.scale.z = 1;

        default:
            break;
        }

        marker.ns = mj_id2name(m, mjtObj::mjOBJ_BODY, body_id);
        marker.color.a = m->geom_rgba[4 * geom_id + 3];
        marker.color.r = m->geom_rgba[4 * geom_id];
        marker.color.g = m->geom_rgba[4 * geom_id + 1];
        marker.color.b = m->geom_rgba[4 * geom_id + 2];
        marker.pose.position.x = d->xpos[3 * body_id];
        marker.pose.position.y = d->xpos[3 * body_id + 1];
        marker.pose.position.z = d->xpos[3 * body_id + 2];
        marker.pose.orientation.x = d->xquat[4 * body_id + 1];
        marker.pose.orientation.y = d->xquat[4 * body_id + 2];
        marker.pose.orientation.z = d->xquat[4 * body_id + 3];
        marker.pose.orientation.w = d->xquat[4 * body_id];
        marker_pub.publish(marker);
    }
}

void MjRos::add_object_state(const int body_id)
{
    mujoco_msgs::ObjectState object_state;
    object_state.name = mj_id2name(m, mjtObj::mjOBJ_BODY, body_id);
    object_state.pose.position.x = d->xpos[3 * body_id];
    object_state.pose.position.y = d->xpos[3 * body_id + 1];
    object_state.pose.position.z = d->xpos[3 * body_id + 2];
    object_state.pose.orientation.x = d->xquat[4 * body_id + 1];
    object_state.pose.orientation.y = d->xquat[4 * body_id + 2];
    object_state.pose.orientation.z = d->xquat[4 * body_id + 3];
    object_state.pose.orientation.w = d->xquat[4 * body_id];

    if (m->body_dofnum[body_id] == 6)
    {
        const int dof_adr = m->jnt_dofadr[m->body_jntadr[body_id]];
        object_state.velocity.linear.x = d->qvel[dof_adr];
        object_state.velocity.linear.y = d->qvel[dof_adr + 1];
        object_state.velocity.linear.z = d->qvel[dof_adr + 2];
        object_state.velocity.angular.x = d->qvel[dof_adr + 3];
        object_state.velocity.angular.y = d->qvel[dof_adr + 4];
        object_state.velocity.angular.z = d->qvel[dof_adr + 5];
    }

    object_states.object_states.push_back(object_state);
}

void MjRos::set_transform(const int body_id, const std::string &object_name)
{
    transform.child_frame_id = object_name;
    transform.transform.translation.x = d->xpos[3 * body_id];
    transform.transform.translation.y = d->xpos[3 * body_id + 1];
    transform.transform.translation.z = d->xpos[3 * body_id + 2];
    transform.transform.rotation.x = d->xquat[4 * body_id + 1];
    transform.transform.rotation.y = d->xquat[4 * body_id + 2];
    transform.transform.rotation.z = d->xquat[4 * body_id + 3];
    transform.transform.rotation.w = d->xquat[4 * body_id];
}

void MjRos::publish_base_pose(const int body_id)
{
    base_pose.pose.pose.position.x = d->xpos[3 * body_id];
    base_pose.pose.pose.position.y = d->xpos[3 * body_id + 1];
    base_pose.pose.pose.position.z = d->xpos[3 * body_id + 2];
    base_pose.pose.pose.orientation.x = d->xquat[4 * body_id + 1];
    base_pose.pose.pose.orientation.y = d->xquat[4 * body_id + 2];
    base_pose.pose.pose.orientation.z = d->xquat[4 * body_id + 3];
    base_pose.pose.pose.orientation.w = d->xquat[4 * body_id];
    base_pose.pose.covariance.assign(0.0);
    base_pose.twist.covariance.assign(0.0);

    base_pose_pub.publish(base_pose);
}