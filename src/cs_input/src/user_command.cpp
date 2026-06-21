// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: user_command (输入源)
// 发布 /<agent_name>/input/user_command      (data)
// 发布 /<agent_name>/input/user_command/desc (desc)
//
// 接收话题 /<agent_name>/master_chat (std_msgs/String) 作为用户指令输入。
// 内部维护消息队列，每条消息带有 UTC 时间戳，超时自动清理。
//
// 参数:
//   agent_name        - 命名空间前缀，默认 "agent"
//   retention_seconds - 消息保留时间(秒)，默认 3600，-1 永不过期
//   publish_rate      - data 话题发布频率(Hz)，默认 1.0

#include <chrono>
#include <deque>
#include <sstream>
#include <ctime>
#include <mutex>
#include <iomanip>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

using namespace std::chrono_literals;

class UserCommandNode : public rclcpp::Node {
public:
  UserCommandNode(const std::string & agent_name, const std::string & source_name)
  : Node(source_name), agent_name_(agent_name), source_name_(source_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<int>("retention_seconds", 3600);
    this->declare_parameter<double>("publish_rate", 1.0);

    retention_seconds_ = this->get_parameter("retention_seconds").as_int();
    double rate = this->get_parameter("publish_rate").as_double();

    // 订阅 master_chat 话题接收用户指令
    command_sub_ = this->create_subscription<std_msgs::msg::String>(
      "/" + agent_name_ + "/master_chat",
      rclcpp::QoS(10).reliable(),
      std::bind(&UserCommandNode::handle_command, this, std::placeholders::_1));

    // data 话题
    data_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/input/" + source_name_, rclcpp::QoS(1).reliable());

    // desc 话题 (transient local)
    rclcpp::QoS desc_qos(1);
    desc_qos.transient_local();
    desc_qos.reliable();
    desc_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/input/" + source_name_ + "/desc", desc_qos);

    // 定时发布 data
    data_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / rate),
      std::bind(&UserCommandNode::publish_data, this));

    // 定时清理过期消息
    cleanup_timer_ = this->create_wall_timer(
      10s, std::bind(&UserCommandNode::cleanup_expired, this));

    // 心跳发布 desc
    desc_timer_ = this->create_wall_timer(
      1s, std::bind(&UserCommandNode::publish_desc, this));

    publish_desc();
    RCLCPP_INFO(this->get_logger(), "user_command 输入源已启动, agent: %s", agent_name_.c_str());
  }

private:
  void publish_desc() {
    std_msgs::msg::String msg;
    msg.data = R"json(
      {
        "name": "user_command",
        "type": "user_input",
        "description": "用户文本指令。通过话题 /<agent_name>/master_chat 发送新命令。内部缓存最多 retention_seconds 秒（默认3600，-1永不丢弃）。当没有新指令时，data 中的列表可能为空。",
        "data_schema": {
          "commands": "array of {text, timestamp_utc}"
        }
      }
    )json";
    desc_pub_->publish(msg);
  }

  void handle_command(const std_msgs::msg::String::SharedPtr msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    CommandEntry entry;
    entry.text = msg->data;

    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm *gmt = std::gmtime(&now_time_t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmt);
    entry.timestamp_utc = buf;

    commands_.push_back(entry);
    RCLCPP_INFO(this->get_logger(), "收到用户指令: %s", entry.text.c_str());
  }

  void publish_data() {
    std_msgs::msg::String msg;
    msg.data = build_commands_json();
    data_pub_->publish(msg);
  }

  void cleanup_expired() {
    if (retention_seconds_ < 0) return;  // 永不过期

    // 计算过期边界时间字符串 (ISO8601 UTC)
    auto now = std::chrono::system_clock::now();
    auto expire_time = now - std::chrono::seconds(retention_seconds_);
    auto expire_time_t = std::chrono::system_clock::to_time_t(expire_time);
    std::tm *expire_tm = std::gmtime(&expire_time_t);
    char expire_buf[64];
    std::strftime(expire_buf, sizeof(expire_buf), "%Y-%m-%dT%H:%M:%SZ", expire_tm);
    std::string expire_str(expire_buf);

    std::lock_guard<std::mutex> lock(mutex_);
    while (!commands_.empty() && commands_.front().timestamp_utc < expire_str) {
      commands_.pop_front();
    }
  }

  std::string build_commands_json() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream json;
    json << "{\"commands\":[";
    bool first = true;
    for (const auto & cmd : commands_) {
      if (!first) json << ",";
      first = false;
      json << "{\"text\":\"" << escape_json(cmd.text) << "\","
           << "\"timestamp_utc\":\"" << cmd.timestamp_utc << "\"}";
    }
    json << "]}";
    return json.str();
  }

  static std::string escape_json(const std::string & s) {
    std::ostringstream o;
    for (char c : s) {
      switch (c) {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:   o << c;
      }
    }
    return o.str();
  }

  struct CommandEntry {
    std::string text;
    std::string timestamp_utc;
  };

  std::string agent_name_;
  std::string source_name_;
  int retention_seconds_;

  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr command_sub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr data_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr desc_pub_;
  rclcpp::TimerBase::SharedPtr data_timer_;
  rclcpp::TimerBase::SharedPtr cleanup_timer_;
  rclcpp::TimerBase::SharedPtr desc_timer_;

  std::deque<CommandEntry> commands_;
  std::mutex mutex_;
};

int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);
  auto temp = std::make_shared<rclcpp::Node>("temp");
  temp->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp->get_parameter("agent_name").as_string();
  temp.reset();

  auto node = std::make_shared<UserCommandNode>(agent_name, "user_command");
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}