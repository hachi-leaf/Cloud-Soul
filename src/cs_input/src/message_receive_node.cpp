// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: /<agent_name>/message_receive_node (输入源节点，由 input_mgmt_node 自动发现)
// 作用: 提供 ROS 2 服务作为消息输入渠道，接收用户文本消息并发布到 data 话题。
//       每条消息自动添加 [UTC时间+渠道] 前缀，渠道名与服务最后一级名称一致。
//
// 参数:
//   agent_name - 命名空间，默认 "agent"
//   ros_channel - ROS 消息渠道服务名后缀，默认 "ros2_msg"
//   info_rate  - info 话题发布频率(Hz)，默认 1.0
//
// 对外接口 (遵循 input 模块规范):
//   话题  /<agent_name>/input/message_receive/info (cs_interfaces/msg/InputInfo)
//        desc: "ros2_msg 消息接收", mode: "accumulate"
//   话题  /<agent_name>/input/message_receive      (std_msgs/String)
//       内容: 带前缀的消息文本，例如 "[2026-06-22T12:34:56Z+ros2_msg] 你好"
//   服务  /<agent_name>/input/message_receive/ros2_msg (SendMessage)
//       请求: string message
//       响应: bool success, string message
//   服务  /<agent_name>/input/message_receive/web_chat (SendMessage)

#include <chrono>
#include <ctime>
#include <sstream>
#include <string>
#include <memory>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/msg/input_info.hpp"
#include "cs_interfaces/srv/send_message.hpp"

using namespace std::chrono_literals;
using SendMessage = cs_interfaces::srv::SendMessage;
using InputInfo = cs_interfaces::msg::InputInfo;

class MessageReceiveNode : public rclcpp::Node {
public:
  MessageReceiveNode(const std::string & agent_name)
  : Node("message_receive_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);
    double info_rate = this->get_parameter("info_rate").as_double();
    this->declare_parameter<std::string>("ros_channel", "ros2_msg");

    channel_name_ = this->get_parameter("ros_channel").as_string();  // 渠道名，可配置，与服务最后一级一致

    // info 话题 (描述 + 心跳 + mode)
    rclcpp::QoS info_qos(1);
    info_qos.transient_local();
    info_qos.reliable();
    info_pub_ = this->create_publisher<InputInfo>(
      "/" + agent_name_ + "/input/message_receive/info", info_qos);

    // data 话题
    data_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/input/message_receive",
      rclcpp::QoS(1).reliable().transient_local());

    // 服务: ros2_msg 渠道
    ros2_msg_srv_ = this->create_service<SendMessage>(
      "/" + agent_name_ + "/input/message_receive/" + channel_name_,
      std::bind(&MessageReceiveNode::handle_ros2_msg, this,
                std::placeholders::_1, std::placeholders::_2));

    // 服务: web_chat 渠道
    web_chat_srv_ = this->create_service<SendMessage>(
      "/" + agent_name_ + "/input/message_receive/web_chat",
      std::bind(&MessageReceiveNode::handle_web_chat, this,
                std::placeholders::_1, std::placeholders::_2));

    // 心跳定时器
    heartbeat_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / info_rate),
      std::bind(&MessageReceiveNode::publish_info, this));

    publish_info();  // 立即发布一次 info，避免启动初期被管理节点判定超时
    RCLCPP_INFO(this->get_logger(), "MessageReceiveNode ready, channels: %s, web_chat", channel_name_.c_str());
  }

private:
  void publish_info() {
    InputInfo msg;
    msg.desc = channel_name_ + " / web_chat 消息接收";
    msg.mode = "accumulate";
    info_pub_->publish(msg);
  }

  void handle_ros2_msg(
      const std::shared_ptr<SendMessage::Request> req,
      std::shared_ptr<SendMessage::Response> res) {
    handle_message(req, res, channel_name_);
  }

  void handle_web_chat(
      const std::shared_ptr<SendMessage::Request> req,
      std::shared_ptr<SendMessage::Response> res) {
    handle_message(req, res, "web_chat");
  }

  void handle_message(
      const std::shared_ptr<SendMessage::Request> req,
      std::shared_ptr<SendMessage::Response> res,
      const std::string & channel) {
    // 构造带前缀的消息
    std::string prefixed = build_prefixed_message(req->message, channel);
    // 发布到 data 话题
    std_msgs::msg::String data_msg;
    data_msg.data = prefixed;
    data_pub_->publish(data_msg);
    // 返回成功响应
    res->success = true;
    res->message = "消息已发送";
  }

  std::string build_prefixed_message(const std::string & body, const std::string & channel) {
    // 获取当前 UTC 时间
    std::time_t now = std::time(nullptr);
    std::tm *gmt = std::gmtime(&now);
    char time_buf[64];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmt);

    std::ostringstream oss;
    oss << "[" << time_buf << "+" << channel << "] " << body;
    return oss.str();
  }

  std::string agent_name_;
  std::string channel_name_;

  rclcpp::Publisher<InputInfo>::SharedPtr info_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr data_pub_;
  rclcpp::Service<SendMessage>::SharedPtr ros2_msg_srv_;
  rclcpp::Service<SendMessage>::SharedPtr web_chat_srv_;
  rclcpp::TimerBase::SharedPtr heartbeat_timer_;
};

int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);

  auto temp = std::make_shared<rclcpp::Node>("temp");
  temp->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp->get_parameter("agent_name").as_string();
  temp.reset();

  auto node = std::make_shared<MessageReceiveNode>(agent_name);
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}