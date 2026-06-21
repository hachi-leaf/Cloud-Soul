// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: <agent_name>_input_mgmt
// 作用: 动态发现 /agent_name/input/<src>/desc 话题，管理输入源状态，聚合后通过服务返回快照。
//
// 每个输入源必须发布 desc 话题（类型任意，std_msgs/String 直接取文本，否则 base64 编码）。
// 输入源可额外发布同名 data 话题，处理方式相同。心跳基于 desc，超时后自动移除。
//
// 参数:
//   agent_name       - 命名空间前缀，默认 "agent"
//   info_timeout     - desc 超时时间 (秒)，默认 3.0
//   discovery_period - 新源扫描周期 (秒)，默认 1.0
//
// 服务:
//   /<agent_name>/input (GetInputSnapshot) 返回所有在线输入源的最新 JSON。
//
// 实现细节:
//   - 使用 GenericSubscription 兼容非 String 类型，String 类型优先用强类型订阅优化性能。
//   - desc 与 data 话题分开管理，互不影响。
//   - desc_text 在放入快照前会经过 compress_json 处理，移除冗余空白符。

#include <regex>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/generic_subscription.hpp"
#include "rclcpp/serialized_message.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/srv/get_input_snapshot.hpp"

using namespace std::chrono_literals;
using GetInputSnapshot = cs_interfaces::srv::GetInputSnapshot;

// -----------------------------------------------------------------------------
// 输入源运行时记录
// -----------------------------------------------------------------------------
struct InputSourceEntry {
  std::string name;
  std::string desc_text;
  std::string data_text;
  rclcpp::Time last_desc_time;
  rclcpp::Time last_data_time;

  // desc 订阅句柄（二选一）
  rclcpp::GenericSubscription::SharedPtr desc_sub_generic;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr desc_sub_string;

  // data 订阅句柄（二选一）
  rclcpp::GenericSubscription::SharedPtr data_sub_generic;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr data_sub_string;

  InputSourceEntry()
    : last_desc_time(0, 0, RCL_ROS_TIME),
      last_data_time(0, 0, RCL_ROS_TIME) {}
};

// -----------------------------------------------------------------------------
// 管理节点
// -----------------------------------------------------------------------------
class InputMgmtNode : public rclcpp::Node {
public:
  InputMgmtNode(const std::string & node_name, const std::string & agent_name)
  : Node(node_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_timeout", 3.0);
    this->declare_parameter<double>("discovery_period", 1.0);

    info_timeout_ = this->get_parameter("info_timeout").as_double();
    double discovery_period = this->get_parameter("discovery_period").as_double();

    topic_prefix_ = "/" + agent_name_ + "/input/";

    // 服务：按需返回聚合快照
    snapshot_srv_ = this->create_service<GetInputSnapshot>(
      "/" + agent_name_ + "/input",
      std::bind(&InputMgmtNode::handle_snapshot, this,
                std::placeholders::_1, std::placeholders::_2));

    // 定时扫描新输入源
    discovery_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(discovery_period),
      std::bind(&InputMgmtNode::discover_inputs, this));

    // 定时清理超时源
    cleanup_timer_ = this->create_wall_timer(
      2s, std::bind(&InputMgmtNode::cleanup_inputs, this));

    RCLCPP_INFO(this->get_logger(), "InputMgmtNode started, agent: %s", agent_name_.c_str());
  }

private:
  // ---------------------------------------------------------------------------
  // 发现新输入源：扫描 /agent_name/input/+/desc 话题
  // ---------------------------------------------------------------------------
  void discover_inputs() {
    auto topics_and_types = this->get_topic_names_and_types();
    std::regex desc_regex(topic_prefix_ + "([^/]+)/desc");
    std::smatch match;

    for (const auto & [topic, types] : topics_and_types) {
      if (std::regex_match(topic, match, desc_regex)) {
        std::string src_name = match[1].str();
        if (sources_.find(src_name) != sources_.end()) {
          continue;  // 已注册
        }

        RCLCPP_INFO(this->get_logger(), "发现新输入源: %s", src_name.c_str());
        auto entry = std::make_shared<InputSourceEntry>();
        entry->name = src_name;
        entry->last_desc_time = this->now();  // 初始时间戳，防止立即超时

        // ---- 订阅 desc 话题 ----
        std::string desc_topic = topic_prefix_ + src_name + "/desc";
        auto desc_types_it = topics_and_types.find(desc_topic);
        if (desc_types_it != topics_and_types.end() && !desc_types_it->second.empty()) {
          const std::string & type = desc_types_it->second[0];
          if (type == "std_msgs/msg/String") {
            entry->desc_sub_string = this->create_subscription<std_msgs::msg::String>(
              desc_topic, rclcpp::QoS(1).transient_local().reliable(),
              [this, src_name](const std_msgs::msg::String::SharedPtr msg) {
                auto it = sources_.find(src_name);
                if (it != sources_.end()) {
                  it->second->desc_text = msg->data;
                  it->second->last_desc_time = this->now();
                }
              });
          } else {
            entry->desc_sub_generic = this->create_generic_subscription(
              desc_topic, type, rclcpp::QoS(1).transient_local().reliable(),
              [this, src_name](std::shared_ptr<rclcpp::SerializedMessage> msg) {
                auto it = sources_.find(src_name);
                if (it != sources_.end()) {
                  auto & rcl_msg = msg->get_rcl_serialized_message();
                  std::string raw(reinterpret_cast<const char*>(rcl_msg.buffer),
                                  rcl_msg.buffer_length);
                  it->second->desc_text = "base64:" + base64_encode(raw);
                  it->second->last_desc_time = this->now();
                }
              });
          }
        }

        // ---- 订阅 data 话题（可选） ----
        std::string data_topic = topic_prefix_ + src_name;
        auto data_types_it = topics_and_types.find(data_topic);
        if (data_types_it != topics_and_types.end() && !data_types_it->second.empty()) {
          const std::string & type = data_types_it->second[0];
          if (type == "std_msgs/msg/String") {
            entry->data_sub_string = this->create_subscription<std_msgs::msg::String>(
              data_topic, rclcpp::QoS(1).reliable(),
              [this, src_name](const std_msgs::msg::String::SharedPtr msg) {
                auto it = sources_.find(src_name);
                if (it != sources_.end()) {
                  it->second->data_text = msg->data;
                  it->second->last_data_time = this->now();
                }
              });
          } else {
            entry->data_sub_generic = this->create_generic_subscription(
              data_topic, type, rclcpp::QoS(1).reliable(),
              [this, src_name](std::shared_ptr<rclcpp::SerializedMessage> msg) {
                auto it = sources_.find(src_name);
                if (it != sources_.end()) {
                  auto & rcl_msg = msg->get_rcl_serialized_message();
                  std::string raw(reinterpret_cast<const char*>(rcl_msg.buffer),
                                  rcl_msg.buffer_length);
                  it->second->data_text = "base64:" + base64_encode(raw);
                  it->second->last_data_time = this->now();
                }
              });
          }
        }

        sources_[src_name] = entry;
      }
    }
  }

  // ---------------------------------------------------------------------------
  // 清理超时输入源
  // ---------------------------------------------------------------------------
  void cleanup_inputs() {
    auto now = this->now();
    std::vector<std::string> to_remove;
    for (const auto & [name, entry] : sources_) {
      double age = (now - entry->last_desc_time).seconds();
      if (age > info_timeout_) {
        to_remove.push_back(name);
      }
    }
    for (const auto & name : to_remove) {
      RCLCPP_WARN(this->get_logger(), "输入源 %s 心跳超时，已移除", name.c_str());
      sources_.erase(name);
    }
  }

  // ---------------------------------------------------------------------------
  // 服务回调：生成当前所有输入源的聚合 JSON
  // ---------------------------------------------------------------------------
  void handle_snapshot(
    const std::shared_ptr<GetInputSnapshot::Request> /*req*/,
    std::shared_ptr<GetInputSnapshot::Response> res)
  {
    cleanup_inputs();  // 保证返回活跃列表

    std::ostringstream json;
    json << "{\"timestamp\":" << this->now().seconds() << ",\"inputs\":[";
    bool first = true;
    for (const auto & [name, entry] : sources_) {
      if (!first) json << ",";
      first = false;

      json << "{\"name\":\"" << escape_json(name) << "\""
           << ",\"description\":\"" << escape_json(compress_json(entry->desc_text)) << "\""
           << ",\"data\":\"" << escape_json(entry->data_text) << "\""
           << ",\"last_desc_update\":" << entry->last_desc_time.seconds();
      if (entry->last_data_time.nanoseconds() > 0) {
        json << ",\"last_data_update\":" << entry->last_data_time.seconds();
      }
      json << "}";
    }
    json << "]}";
    res->snapshot_json = json.str();
  }

  // ---------------------------------------------------------------------------
  // 移除 JSON 字符串中非必要的空白字符（保留字符串内的空白）
  // ---------------------------------------------------------------------------
  static std::string compress_json(const std::string & json_str) {
    std::string out;
    bool in_string = false;
    bool escape = false;
    for (size_t i = 0; i < json_str.size(); ++i) {
      char c = json_str[i];
      if (in_string) {
        out += c;
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == '"') {
          in_string = false;
        }
      } else {
        if (c == '"') {
          in_string = true;
          out += c;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          // 跳过字符串外的空白
          continue;
        } else {
          out += c;
        }
      }
    }
    return out;
  }

  // ---------------------------------------------------------------------------
  // 简易 JSON 字符串转义（处理控制字符和引号）
  // ---------------------------------------------------------------------------
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

  // ---------------------------------------------------------------------------
  // Base64 编码（用于非 String 类型消息）
  // ---------------------------------------------------------------------------
  static std::string base64_encode(const std::string & in) {
    static const char table[] = 
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
      val = (val << 8) + c;
      valb += 8;
      while (valb >= 0) {
        out.push_back(table[(val >> valb) & 0x3F]);
        valb -= 6;
      }
    }
    if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
  }

  // ---------------------------------------------------------------------------
  // 成员变量
  // ---------------------------------------------------------------------------
  std::string agent_name_;
  std::string topic_prefix_;
  double info_timeout_;

  rclcpp::Service<GetInputSnapshot>::SharedPtr snapshot_srv_;
  rclcpp::TimerBase::SharedPtr discovery_timer_;
  rclcpp::TimerBase::SharedPtr cleanup_timer_;

  std::unordered_map<std::string, std::shared_ptr<InputSourceEntry>> sources_;
};

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);

  auto temp_node = std::make_shared<rclcpp::Node>("temp");
  temp_node->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp_node->get_parameter("agent_name").as_string();
  temp_node.reset();

  std::string node_name = agent_name + "_input_mgmt";
  auto node = std::make_shared<InputMgmtNode>(node_name, agent_name);

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}