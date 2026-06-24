// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: /<agent_name>/input_mgmt_node
// 作用: 自动发现并管理 /<agent_name>/input/<src> 输入源，提供事件累积快照服务。
//       管理节点缓存自上次快照调用以来每个输入源发布的所有数据消息 (非仅最新一条)。
//       快照服务被调用时，将所有累积的数据按源名分组，以 JSON 数组形式返回，
//       并清空内部缓存，开始下一轮累积。
//       心跳基于 /<agent_name>/input/<src>/info 话题，超时自动移除输入源 (同时丢弃其缓存)。
//
// 参数:
//   agent_name       - 命名空间，默认 "agent"
//   info_timeout     - info 心跳超时(秒)，默认 3.0
//   discovery_period - 新源扫描周期(秒)，默认 1.0
//
// 对外接口:
//   服务  /<agent_name>/input (GetSnapshot)  返回自上次调用以来累积的输入数据快照 JSON
//
// 子 input 模块开发规范:
//   1. 发布描述与心跳: 话题 /<agent_name>/input/<src>/info
//      消息类型 cs_interfaces/msg/InputInfo, QoS: transient_local + reliable, keep_last(1)
//      字段 desc: 固定描述字符串, mode: "accumulate" (累积) 或 "latest" (仅最新)
//      需周期性发布以维持心跳 (周期 ≤ info_timeout)
//
//   2. 发布数据: 话题 /<agent_name>/input/<src>
//      消息类型 std_msgs/String, QoS: reliable + transient_local, keep_last(1)
//      内容为任意格式的文本数据 (例如 "你好" 或 "CPU: 12%")
//      每次发布都将被管理节点存入内部缓存，直到下一次快照服务调用时集中返回。
//      因此对于事件型输入 (如用户消息)，不必担心消息被覆盖。
//      输入源可自行决定发布频率和内容格式。
//
//   快照返回示例 (假设调用前收到了两条系统负载数据和两条用户消息):
//     {
//       "system_load": ["CPU: 12% Mem: 34%", "CPU: 15% Mem: 34%"],
//       "user_chat": ["你好", "继续"]
//     }
//     若无任何新数据，返回空对象 {}。

#include <chrono>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <sstream>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/msg/input_info.hpp"
#include "cs_interfaces/srv/get_snapshot.hpp"
#include "cs_interfaces/constants.hpp"

using namespace std::chrono_literals;
using GetSnapshot = cs_interfaces::srv::GetSnapshot;
using InputInfo = cs_interfaces::msg::InputInfo;

// ------------------------------------------------------------------
// 输入源运行时记录
// ------------------------------------------------------------------
struct InputEntry {
  std::string name;
  std::string info_text;              // 描述
  std::string mode;                   // "accumulate" 或 "latest"，默认 "accumulate"
  std::vector<std::string> pending_data;  // 自上次快照以来累积的数据消息
  rclcpp::Time last_info_time;
  rclcpp::Time last_data_time;        // 最近一次收到 data 的时间 (仅记录用)

  rclcpp::Subscription<InputInfo>::SharedPtr info_sub;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr data_sub;

  InputEntry() : mode("accumulate"),
                 last_info_time(0, 0, RCL_ROS_TIME),
                 last_data_time(0, 0, RCL_ROS_TIME) {}
};

// ------------------------------------------------------------------
// 输入管理节点
// ------------------------------------------------------------------
class InputMgmtNode : public rclcpp::Node {
public:
  InputMgmtNode(const std::string & node_name, const std::string & agent_name)
    : Node(node_name, agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_timeout", 3.0);
    this->declare_parameter<double>("discovery_period", 1.0);

    info_timeout_ = this->get_parameter("info_timeout").as_double();
    double discovery_period = this->get_parameter("discovery_period").as_double();

    topic_prefix_ = "/" + agent_name_ + "/input/";

    // 创建互斥回调组，保证所有对 sources_ 的操作串行化
    callback_group_ = this->create_callback_group(
        rclcpp::CallbackGroupType::MutuallyExclusive);

    // 创建快照服务
    snapshot_srv_ = this->create_service<GetSnapshot>(
      "/" + agent_name_ + "/input",
      std::bind(&InputMgmtNode::handle_snapshot, this,
                std::placeholders::_1, std::placeholders::_2),
      rmw_qos_profile_services_default,
      callback_group_);

    // 定时扫描新输入源
    discovery_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(discovery_period),
      std::bind(&InputMgmtNode::discover_inputs, this),
      callback_group_);

    // 定时清理超时输入源
    cleanup_timer_ = this->create_wall_timer(
      cloud_soul::CLEANUP_INTERVAL,
      std::bind(&InputMgmtNode::cleanup_inputs, this),
      callback_group_);

    RCLCPP_INFO(this->get_logger(), "InputMgmtNode started, agent: %s", agent_name_.c_str());
  }

private:
  // ----------------------------------------------------------------
  // 发现新输入源: 扫描 /<agent>/input/+/info 话题
  // ----------------------------------------------------------------
  void discover_inputs() {
      auto topics_and_types = this->get_topic_names_and_types();
      std::regex info_regex(topic_prefix_ + "([^/]+)/info");
      std::smatch match;

      for (const auto & [topic, types] : topics_and_types) {
          if (std::regex_match(topic, match, info_regex)) {
              std::string src_name = match[1].str();

              // 跳过已注册的输入源
              if (sources_.find(src_name) != sources_.end()) {
                  continue;
              }

              // 关键修复：如果 info 话题没有活跃的发布者，则忽略该话题，
              // 避免将已销毁但话题残留的“僵尸源”重新注册。
              if (this->count_publishers(topic) == 0) {
                  continue;
              }

              RCLCPP_INFO(this->get_logger(), "发现新输入源: %s", src_name.c_str());
              auto entry = std::make_shared<InputEntry>();
              entry->name = src_name;
              entry->last_info_time = this->now();  // 初始时间戳，避免立即被心跳超时清理

              // 订阅 info 话题 (描述 + 心跳 + mode)
              rclcpp::SubscriptionOptions info_opts;
              info_opts.callback_group = callback_group_;
              entry->info_sub = this->create_subscription<InputInfo>(
                  topic_prefix_ + src_name + "/info",
                  rclcpp::QoS(1000).transient_local().reliable(),
                  [this, src_name](const InputInfo::SharedPtr msg) {
                      auto it = sources_.find(src_name);
                      if (it != sources_.end()) {
                          it->second->info_text = msg->desc;
                          it->second->mode = msg->mode.empty() ? "accumulate" : msg->mode;
                          it->second->last_info_time = this->now();
                      }
                  },
                  info_opts);

              // 订阅 data 话题 (累积所有数据消息)
              rclcpp::SubscriptionOptions data_opts;
              data_opts.callback_group = callback_group_;
              entry->data_sub = this->create_subscription<std_msgs::msg::String>(
                  topic_prefix_ + src_name,
                  rclcpp::QoS(1000).reliable().transient_local(),
                  [this, src_name](const std_msgs::msg::String::SharedPtr msg) {
                      auto it = sources_.find(src_name);
                      if (it != sources_.end()) {
                          // 根据 mode 决定是否清空后再追加 (latest 模式只保留最新)
                          if (it->second->mode == "latest") {
                              it->second->pending_data.clear();
                          }
                          it->second->pending_data.push_back(msg->data);
                          it->second->last_data_time = this->now();
                      }
                  },
                  data_opts);

              sources_[src_name] = entry;
          }
      }
  }

  // ----------------------------------------------------------------
  // 清理心跳超时的输入源 (同时丢弃其缓存数据)
  // ----------------------------------------------------------------
  void cleanup_inputs() {
      auto now = this->now();
      std::vector<std::string> to_remove;
      for (const auto & [name, entry] : sources_) {
          // 1. 立即移除：发布者已消失
          std::string info_topic = topic_prefix_ + name + "/info";
          if (this->count_publishers(info_topic) == 0) {
              RCLCPP_WARN(this->get_logger(), "输入源 %s 的发布者已消失，移除", name.c_str());
              to_remove.push_back(name);
              continue;
          }
          // 2. 兜底：心跳超时（防止 DDS 残留导致计数不准）
          if ((now - entry->last_info_time).seconds() > info_timeout_) {
              RCLCPP_WARN(this->get_logger(), "输入源 %s 心跳超时，移除", name.c_str());
              to_remove.push_back(name);
          }
      }
      for (const auto & name : to_remove) {
          sources_.erase(name);
      }
  }
  // ----------------------------------------------------------------
  // 快照服务回调: 返回自上次调用以来累积的所有数据，并清空内部缓存
  // ----------------------------------------------------------------
  void handle_snapshot(
      const std::shared_ptr<GetSnapshot::Request> /*req*/,
      std::shared_ptr<GetSnapshot::Response> res) {
    // 先清理超时源
    cleanup_inputs();

    std::ostringstream json;
    json << "{";
    bool first = true;

    for (auto & [name, entry] : sources_) {
      if (entry->pending_data.empty()) {
        continue; // 该源无新数据，跳过
      }

      // 构造 JSON 键值对: "源名": ["数据1", "数据2", ...]
      if (!first) json << ",";
      first = false;

      json << "\"" << escape_json(name) << "\":[";
      for (size_t i = 0; i < entry->pending_data.size(); ++i) {
        if (i > 0) json << ",";
        json << "\"" << escape_json(entry->pending_data[i]) << "\"";
      }
      json << "]";

      // 清空该源的累积数据，准备下一轮
      entry->pending_data.clear();
    }

    json << "}";
    res->snapshot_json = json.str();
  }

  // ----------------------------------------------------------------
  // JSON 字符串转义 (仅处理双引号、反斜杠及常用控制字符)
  // ----------------------------------------------------------------
  static std::string escape_json(const std::string & s) {
    std::ostringstream o;
    for (char c : s) {
      switch (c) {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n";  break;
        case '\r': o << "\\r";  break;
        case '\t': o << "\\t";  break;
        default:   o << c;
      }
    }
    return o.str();
  }

  // ----------------------------------------------------------------
  // 成员变量
  // ----------------------------------------------------------------
  std::string agent_name_;
  std::string topic_prefix_;
  double info_timeout_;

  rclcpp::CallbackGroup::SharedPtr callback_group_;
  rclcpp::Service<GetSnapshot>::SharedPtr snapshot_srv_;
  rclcpp::TimerBase::SharedPtr discovery_timer_;
  rclcpp::TimerBase::SharedPtr cleanup_timer_;

  std::unordered_map<std::string, std::shared_ptr<InputEntry>> sources_;
};

// ------------------------------------------------------------------
// main
// ------------------------------------------------------------------
int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);

  // 从参数服务器获取 agent_name 命名空间
  auto temp_node = std::make_shared<rclcpp::Node>("temp");
  temp_node->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp_node->get_parameter("agent_name").as_string();
  temp_node.reset();

  std::string node_name = "input_mgmt_node";
  auto node = std::make_shared<InputMgmtNode>(node_name, agent_name);

  // 使用多线程执行器，互斥回调组内的操作串行，但不影响其他节点
  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}