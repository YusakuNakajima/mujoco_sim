// Copyright (c) 2023, Hoang Giang Nguyen - Institute for Artificial Intelligence, University Bremen

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

#include <ros/ros.h>

#include <string>
#include <chrono>
#include <iostream>
#include <jsoncpp/json/json.h>
#include <jsoncpp/json/reader.h>
#include <thread>
#include <mutex>
#include <csignal>

#include <zmq.hpp>

#include "state_msgs.cpp"

using namespace std::chrono_literals;

std::vector<std::thread> workers;

std::mutex mtx;

std::map<std::string, std::map<std::string, std::pair<std::vector<double>, bool>>> send_objects;

bool should_shut_down = false;

class StateHandle
{
public:
    StateHandle(const std::string &socket_addr) : socket_addr(socket_addr)
    {
        socket_server = zmq::socket_t(context, zmq::socket_type::rep);
        socket_server.bind(socket_addr);
        ROS_INFO("Bind server socket to address %s", socket_addr.c_str());
    }

    ~StateHandle()
    {
        socket_server.unbind(socket_addr);
        ROS_INFO("Unbind server socket from address %s", socket_addr.c_str());

        if (send_buffer != nullptr)
        {
            free(send_buffer);
        }

        if (receive_buffer != nullptr)
        {
            free(receive_buffer);
        }
    }

public:
    void communicate()
    {
        // Receive JSON string over ZMQ
        zmq::message_t request_meta_data;
        socket_server.recv(request_meta_data, zmq::recv_flags::none);

        Json::Reader reader;
        reader.parse(request_meta_data.to_string(), meta_data_json);
        std::cout << meta_data_json.toStyledString() << std::endl;

        std::vector<double *> send_data_vec;
        std::vector<double *> receive_data_vec;
        bool is_received_data_sent = false;

    receive_meta_data:
        Json::Value send_objects_json = meta_data_json["send"];

        for (auto it = send_objects_json.begin(); it != send_objects_json.end(); ++it)
        {
            const std::string object_name = it.key().asString();
            mtx.lock();
            send_objects[object_name] = {};
            for (const Json::Value &attribute_json : *it)
            {
                const std::string attribute_name = attribute_json.asString();
                send_objects[object_name][attribute_name] = {attribute_map[attribute_name], false};
                for (double &value : send_objects[object_name][attribute_name].first)
                {
                    send_data_vec.push_back(&value);
                }
            }
            mtx.unlock();
        }

        Json::Value receive_objects_json = meta_data_json["receive"];

        for (auto it = receive_objects_json.begin(); it != receive_objects_json.end(); ++it)
        {
            const std::string object_name = it.key().asString();
            for (const Json::Value &attribute_json : *it)
            {
                const std::string attribute_name = attribute_json.asString();
                int start = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                while ((send_objects.count(object_name) == 0 || send_objects[object_name].count(attribute_name) == 0) && ros::ok())
                {
                    const int now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (now - start > 1)
                    {
                        ROS_INFO("[Socket %s]: Waiting for [%s][%s] to be declared", socket_addr.c_str(), object_name.c_str(), attribute_name.c_str());
                        start = now;
                    }
                }
            }

            for (const Json::Value &attribute_json : *it)
            {
                const std::string attribute_name = attribute_json.asString();
                mtx.lock();
                for (double &value : send_objects[object_name][attribute_name].first)
                {
                    receive_data_vec.push_back(&value);
                }
                mtx.unlock();
            }
        }

        const size_t send_buffer_size = 1 + send_data_vec.size();
        const size_t receive_buffer_size = 1 + receive_data_vec.size();

        // Send buffer sizes over ZMQ
        size_t buffer[2] = {send_buffer_size, receive_buffer_size};
        zmq::message_t reply_meta_data(sizeof(buffer));
        memcpy(reply_meta_data.data(), buffer, sizeof(buffer));
        socket_server.send(reply_meta_data, zmq::send_flags::none);

        send_buffer = (double *)calloc(send_buffer_size, sizeof(double));
        receive_buffer = (double *)calloc(receive_buffer_size, sizeof(double));

        while (ros::ok())
        {
            // Receive send_data over ZMQ
            zmq::message_t request_data;
            socket_server.recv(request_data);

            if (request_data.to_string()[0] == '{')
            {
                Json::Reader reader;
                reader.parse(request_data.to_string(), meta_data_json);
                std::cout << meta_data_json.toStyledString() << std::endl;

                send_data_vec.clear();
                receive_data_vec.clear();

                goto receive_meta_data;
            }

            memcpy(send_buffer, request_data.data(), send_buffer_size * sizeof(double));

            const double delay_ms = (std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - *send_buffer) / 1000.0;

            for (size_t i = 0; i < send_buffer_size - 1; i++)
            {
                *send_data_vec[i] = send_buffer[i + 1];
            }

            if (!is_received_data_sent)
            {
                int start = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                for (auto it = send_objects_json.begin(); it != send_objects_json.end(); ++it)
                {
                    const std::string object_name = it.key().asString();
                    for (const Json::Value &attribute_json : *it)
                    {
                        const std::string attribute_name = attribute_json.asString();
                        send_objects[object_name][attribute_name].second = true;
                    }
                }

                for (auto it = receive_objects_json.begin(); it != receive_objects_json.end(); ++it)
                {
                    const std::string object_name = it.key().asString();
                    for (const Json::Value &attribute_json : *it)
                    {
                        const std::string attribute_name = attribute_json.asString();
                        int start = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        while ((send_objects.count(object_name) == 0 || send_objects[object_name].count(attribute_name) == 0 || !send_objects[object_name][attribute_name].second) && ros::ok())
                        {
                            const int now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                            if (now - start > 1)
                            {
                                ROS_INFO("[Socket %s]: Waiting for data of [%s][%s] to be sent", socket_addr.c_str(), object_name.c_str(), attribute_name.c_str());
                                start = now;
                            }
                        }
                    }
                }

                is_received_data_sent = true;
            }

            *receive_buffer = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (should_shut_down)
            {
                *receive_buffer = -1.0;
            }

            for (size_t i = 0; i < receive_buffer_size - 1; i++)
            {
                receive_buffer[i + 1] = *receive_data_vec[i];
            }

            // Send receive_data over ZMQ
            zmq::message_t reply_data(receive_buffer_size * sizeof(double));
            memcpy(reply_data.data(), receive_buffer, receive_buffer_size * sizeof(double));
            socket_server.send(reply_data, zmq::send_flags::none);

            if (should_shut_down)
            {
                return;
            }
        }
    }

private:
    zmq::context_t context{1};

    std::string socket_addr;

    zmq::socket_t socket_server;

    Json::Value meta_data_json;

    double *send_buffer;

    double *receive_buffer;
};

void worker(int port)
{
    StateHandle state_handle("tcp://127.0.0.1:" + std::to_string(port));
    state_handle.communicate();
}

int main(int argc, char **argv)
{
    // register signal SIGINT and signal handler  
    signal(SIGINT, [](int signum)
    {
        ROS_INFO("Interrupt signal (%d) received.", signum);
        should_shut_down = true;
    }); 

    ros::init(argc, argv, "state_server");
    
    for (size_t thread_num = 0; thread_num < argc - 1; thread_num++)
    {
        std::thread worker_thread(worker, std::stoi(argv[thread_num + 1]));
        worker_thread.detach();
    }

    while (!should_shut_down)
    {
        
    }
    zmq_sleep(1);

    return 0;
}