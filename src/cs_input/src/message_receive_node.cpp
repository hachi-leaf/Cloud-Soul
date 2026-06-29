// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul 标准 input 源节点：消息接收
// unified message receiver for Cloud-Soul
//
// Node: /<agent_name>/message_receive_node
//
// 作用:
//   提供 ROS 2 服务作为消息输入渠道，接收用户文本消息，
//   自动添加 [UTC时间+渠道] 前缀后发布到 data 话题。
//   支持两个渠道：可配置的 ros_channel 和 web_chat_channel。
//   info 话题定期发布描述文本与心跳。
//
// 参数:
//   <string>agent_name         - Agent 命名空间，默认 "agent"
//   <string>ros_channel         - ROS 消息渠道服务名后缀，默认 "ros2_msg"
//                                  必须为非空且仅包含字母、数字、下划线（遵守 ROS 2 名称规范），
//                                  否则节点启动失败并退出。
//   <string>web_chat_channel    - Web 聊天渠道服务名后缀，默认 "web_chat"
//                                  校验规则同 ros_channel。
//   <float64>info_rate           - info 话题发布频率（Hz），默认 1.0
//                                  ≤0 时使用默认 1.0 并发出警告。
//
// 对外接口（遵循 input 模块规范）：
//   话题 /<agent_name>/input/message_receive/info  (std_msgs/String, JSON)
//     内容: {"desc":"<ros_channel> / <web_chat_channel> 消息接收","mode":"accumulate"}
//     QoS: transient_local + reliable
//
//   话题 /<agent_name>/input/message_receive       (std_msgs/String, 纯文本)
//     内容: "[YYYY-MM-DDTHH:MM:SSZ+<渠道>] <消息正文>"
//
//   服务 /<agent_name>/input/message_receive/<ros_channel> (cs_interfaces::SendMessage)
//       请求: string message
//       响应: bool success=true, string message="消息已发送"
//
//   服务 /<agent_name>/input/message_receive/<web_chat_channel> (cs_interfaces::SendMessage)
//       请求/响应同上
//
// 行为特性:
//   1. 服务调用立即处理，无队列，并发安全（单线程）。
//   2. 消息正文可为空或含任何字符，直接拼接前缀。
//   3. 时间戳使用 UTC，格式固定。
//   4. ros_channel / web_chat_channel 参数严格校验：非空且仅允许字母、数字、下划线；
//      非法时节点启动失败并退出，不创建任何服务或话题。
//   5. info_rate ≤ 0 时使用默认 1.0 Hz 并发出警告。
//   6. info 话题定期发布（频率由 info_rate 决定），节点启动立即发布一次。

#include <chrono>
#include <ctime>
#include <sstream>
#include <string>
#include <memory>
#include <cctype>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/srv/send_message.hpp"
#include "nlohmann/json.hpp"

using namespace std::chrono_literals;
using SendMessage = cs_interfaces::srv::SendMessage;
using json = nlohmann::json;

class MessageReceiveNode : public rclcpp::Node {
public:
    explicit MessageReceiveNode(const std::string & agent_name)
        : Node("message_receive_node", agent_name), agent_name_(agent_name)
    {
        this->declare_parameter<std::string>("agent_name", agent_name);
        this->declare_parameter<double>("info_rate", 1.0);
        this->declare_parameter<std::string>("ros_channel", "ros2_msg");
        this->declare_parameter<std::string>("web_chat_channel", "web_chat");

        double info_rate = this->get_parameter("info_rate").as_double();
        if (info_rate <= 0.0) {
            RCLCPP_WARN(this->get_logger(), "Invalid info_rate %.2f, using 1.0", info_rate);
            info_rate = 1.0;
        }

        ros_channel_ = this->get_parameter("ros_channel").as_string();
        web_chat_channel_ = this->get_parameter("web_chat_channel").as_string();

        // ---- 校验 ros_channel 合法性 (ROS 2 名称规则：字母、数字、下划线) ----
        auto validate_channel = [this](const std::string & name, const std::string & param_name) {
            if (name.empty()) {
                RCLCPP_ERROR(this->get_logger(), "%s cannot be empty", param_name.c_str());
                throw std::runtime_error(param_name + " is empty");
            }
            for (char c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                    RCLCPP_ERROR(this->get_logger(),
                                 "%s contains invalid character: '%c'", param_name.c_str(), c);
                    throw std::runtime_error(
                        param_name + " contains invalid character: " + std::string(1, c));
                }
            }
        };
        validate_channel(ros_channel_, "ros_channel");
        validate_channel(web_chat_channel_, "web_chat_channel");

        std::string ns = "/" + agent_name_;

        // ---- 发布器 ----
        rclcpp::QoS qos(1);
        qos.transient_local();
        qos.reliable();
        info_pub_ = this->create_publisher<std_msgs::msg::String>(
            ns + "/input/message_receive/info", qos);

        data_pub_ = this->create_publisher<std_msgs::msg::String>(
            ns + "/input/message_receive",
            rclcpp::QoS(1).reliable().transient_local());

        // ---- 服务 ----
        ros2_msg_srv_ = this->create_service<SendMessage>(
            ns + "/input/message_receive/" + ros_channel_,
            std::bind(&MessageReceiveNode::handle_ros2_msg, this,
                      std::placeholders::_1, std::placeholders::_2));

        web_chat_srv_ = this->create_service<SendMessage>(
            ns + "/input/message_receive/" + web_chat_channel_,
            std::bind(&MessageReceiveNode::handle_web_chat, this,
                      std::placeholders::_1, std::placeholders::_2));

        // ---- 心跳定时器 ----
        heartbeat_timer_ = this->create_wall_timer(
            std::chrono::duration<double>(1.0 / info_rate),
            std::bind(&MessageReceiveNode::publish_info, this));

        // 立即发布一次 info
        publish_info();
        RCLCPP_INFO(this->get_logger(),
                    "MessageReceiveNode ready, channels: %s, %s",
                    ros_channel_.c_str(), web_chat_channel_.c_str());
    }

private:
    void publish_info() {
        json info;
        info["desc"] = ros_channel_ + " / " + web_chat_channel_ + " 消息接收";
        info["mode"] = "accumulate";
        auto msg = std_msgs::msg::String();
        try {
            msg.data = info.dump();
            info_pub_->publish(msg);
        } catch (const std::exception & e) {
            RCLCPP_ERROR(this->get_logger(), "Failed to publish info: %s", e.what());
        }
    }

    void handle_ros2_msg(
        const std::shared_ptr<SendMessage::Request> req,
        std::shared_ptr<SendMessage::Response> res) {
        handle_message(req, res, ros_channel_);
    }

    void handle_web_chat(
        const std::shared_ptr<SendMessage::Request> req,
        std::shared_ptr<SendMessage::Response> res) {
        handle_message(req, res, web_chat_channel_);
    }

    void handle_message(
        const std::shared_ptr<SendMessage::Request> req,
        std::shared_ptr<SendMessage::Response> res,
        const std::string & channel) {
        std::string prefixed = build_prefixed_message(req->message, channel);

        auto data_msg = std_msgs::msg::String();
        data_msg.data = prefixed;
        try {
            data_pub_->publish(data_msg);
        } catch (const std::exception & e) {
            RCLCPP_ERROR(this->get_logger(), "Failed to publish data: %s", e.what());
        }

        res->success = true;
        res->message = "消息已发送";
    }

    std::string build_prefixed_message(const std::string & body,
                                       const std::string & channel) {
        std::time_t now = std::time(nullptr);
        std::tm *gmt = std::gmtime(&now);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmt);

        std::ostringstream oss;
        oss << "[" << time_buf << "+" << channel << "] " << body;
        return oss.str();
    }

    std::string agent_name_;
    std::string ros_channel_;
    std::string web_chat_channel_;

    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr data_pub_;
    rclcpp::Service<SendMessage>::SharedPtr ros2_msg_srv_;
    rclcpp::Service<SendMessage>::SharedPtr web_chat_srv_;
    rclcpp::TimerBase::SharedPtr heartbeat_timer_;
};

int main(int argc, char ** argv) {
    rclcpp::init(argc, argv);

    try {
        auto temp = std::make_shared<rclcpp::Node>("temp");
        temp->declare_parameter<std::string>("agent_name", "agent");
        std::string agent_name = temp->get_parameter("agent_name").as_string();
        temp.reset();

        auto node = std::make_shared<MessageReceiveNode>(agent_name);
        rclcpp::spin(node);
        rclcpp::shutdown();
    } catch (const std::exception & e) {
        std::cerr << "Node startup failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}