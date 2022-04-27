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

#include "mj_model.h"

#include <map>
#include <vector>

bool load_tmp_model(bool reset = true);

struct MimicJoint
{
    std::string from_joint;
    mjtNum multiplier;
    mjtNum offset;
};

class MjSim
{
public:
    MjSim() = default;

    ~MjSim();

public:
    /**
     * @brief Initialize the simulation
     *
     */
    void init();

    /**
     * @brief Implement the controller
     *
     */
    void controller();

    /**
     * @brief Set the mimic joint positions
     * 
     */
    void set_mimic_joints();

    /**
     * @brief Set the odom joint velocities
     * 
     */
    void set_odom_joints();

public:
    /**
     * @brief Spawn new data from file
     *
     */
    static bool add_data(); 

    /**
     * @brief Remove bodies with name
     * 
     * @param body_names Array of body names
     * @return true Successfully removed
     * @return false Fail to remove
     */
    static bool remove_body(const std::vector<std::string> &body_names);

public:
    static std::vector<std::string> joint_names;

    static std::map<std::string, std::pair<std::string, mjtNum>> odom_joints;

    static std::map<std::string, MimicJoint> mimic_joints;

    static std::vector<std::string> link_names;

    static mjtNum sim_start;

    static mjtNum *tau;    
};