// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul 核心管理节点：统一输入管理器
// unified input management node for Cloud-Soul

// Node: /<agent_name>/input_mgmt_node
// Param:
//  <string>agent_name        --> Agent 名
//  <float64>info_timeout     --> 输入源心跳超时时间（秒），默认 3.0
//  <float64>discovery_period --> 输入源发现检查周期（秒），默认 1.0

// Service: /<agent_name>/input
// Struct:
//  Request  <>                  --> 空
//  ---
//  Response <string>snapshot_json --> 累积数据快照 JSON，格式见下文

// 订阅话题（动态发现）：
//   /<agent_name>/input/<src>/info   (std_msgs/String, JSON)
//      内容: {"desc":"...", "mode":"accumulate|latest", "status":"ok|partial_failure"}
//      QoS: transient_local + reliable
//   /<agent_name>/input/<src>        (std_msgs/String, 任意文本)
//      QoS: reliable + transient_local

// 子 input 模块开发规范：
//   1. 每个输入源必须发布 info 话题，用于描述自身并维持心跳。
//      info 话题消息类型为 std_msgs/String，内容为 JSON 字符串。
//      必须字段: desc (描述文本), mode (数据模式, "accumulate" 或 "latest")。
//      可选字段: status (健康状态, "ok" 或 "partial_failure")。
//      需周期性发布（周期 ≤ info_timeout）以维持心跳。
//
//   2. 每个输入源还需发布 data 话题，消息类型为 std_msgs/String，内容为任意格式的文本数据。
//      管理节点会根据 info 中的 mode 决定缓存策略：
//        - "accumulate"（默认）：每次 data 消息追加到内部缓存队列。
//        - "latest"：清空已有缓存，仅保留最新一条 data 消息。
//
//   3. 输入源无需关心数据被如何消费。管理节点提供快照服务，集中返回所有输入源自上次快照以来
//      累积的所有数据（或最新数据），并清空缓存。

// 快照服务返回示例 (snapshot_json 字符串)：
//   {
//     "system_status": ["{\"cpu\":12,...}", "{\"cpu\":15,...}"],
//     "message_receive": ["hello", "world"]
//   }
//   若无任何新数据，返回空对象 {}。

// 行为特性：
//  1. 输入源发现：周期性扫描 /<agent_name>/input/*/info 话题，发现新发布者时自动注册。
//     仅当 info 话题存在活跃发布者（count_publishers > 0）时才注册，避免僵尸话题。
//  2. 心跳超时清理：若已注册的输入源在 info_timeout 秒内未更新 info，则将其移除并丢弃其缓存数据。
//     同时检查发布者存活状态，若发布者已消失则立即移除。
//  3. 模式处理：
//     - accumulate：数据回调中将新消息追加到 pending_data 队列。
//     - latest：先清空 pending_data，再追加新消息，确保只有最新一条。
//     - 若 info JSON 中 mode 字段缺失或为空，默认视为 "accumulate"。
//  4. 快照服务：
//     - 服务调用时，先执行一次清理（cleanup_inputs），移除超时源。
//     - 遍历所有已注册输入源，将 pending_data 非空的源打包为 JSON 对象返回。
//     - 返回后立即清空对应源的 pending_data，开始下一轮累积。
//     - 服务响应仅包含 snapshot_json 字段，无额外错误码。
//  5. JSON 构建：使用 nlohmann::json 库构建返回 JSON，自动处理字符串转义，保证特殊字符安全。
//  6. info JSON 解析防御：若 info 消息内容不是合法 JSON，忽略该次更新并记录警告。
//     该源的心跳时间不更新，最终会因超时被移除，避免管理节点自身崩溃。
//  7. 参数防御：
//     - info_timeout ≤ 0 时自动使用默认值 3.0 秒并警告。
//     - discovery_period ≤ 0 时自动使用默认值 1.0 秒并警告。
//  8. 线程安全：所有对 sources_ 的访问均通过互斥回调组（MutuallyExclusive）串行化，
//     避免数据竞争。多线程执行器仅用于处理其他节点或系统回调。
//  9. 节点关闭：使用标准 rclcpp::shutdown() 流程，无特殊退出逻辑。
// 10. 无外部依赖自定义消息：info 和 data 均使用 std_msgs/String，仅服务使用 cs_interfaces/srv/GetSnapshot。
//     JSON 解析使用 nlohmann/json 库。

#include <chrono>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <sstream>
#include <exception>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/srv/get_snapshot.hpp"
#include "nlohmann/json.hpp"

using namespace std::chrono_literals;
using GetSnapshot = cs_interfaces::srv::GetSnapshot;
using json = nlohmann::json;

constexpr auto CLEANUP_INTERVAL = 1s; // 清理检查周期

// ------------------------------------------------------------------
// 输入源运行时记录
// ------------------------------------------------------------------
struct InputEntry {
    std::string name;                     // 源名称（从话题路径提取）
    std::string desc;                     // 描述文本
    std::string mode = "accumulate";      // "accumulate" 或 "latest"
    std::string status = "ok";            // 健康状态
    std::vector<std::string> pending_data; // 累积的数据消息
    rclcpp::Time last_info_time;          // 最后一次收到 info 的时间

    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr info_sub;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr data_sub;

    InputEntry() : last_info_time(0, 0, RCL_ROS_TIME) {}
};

// ------------------------------------------------------------------
// 输入管理节点
// ------------------------------------------------------------------
class InputMgmtNode : public rclcpp::Node {
public:
    explicit InputMgmtNode(const std::string& agent_name)
        : Node("input_mgmt_node", agent_name), agent_name_(agent_name)
    {
        // 声明并读取参数
        declare_parameter<std::string>("agent_name", agent_name);
        declare_parameter<double>("info_timeout", 3.0);
        declare_parameter<double>("discovery_period", 1.0);

        info_timeout_ = get_parameter("info_timeout").as_double();
        if (info_timeout_ <= 0.0) {
            RCLCPP_WARN(get_logger(), "Invalid info_timeout, using 3.0");
            info_timeout_ = 3.0;
        }

        double discovery_period = get_parameter("discovery_period").as_double();
        if (discovery_period <= 0.0) {
            RCLCPP_WARN(get_logger(), "Invalid discovery_period, using 1.0");
            discovery_period = 1.0;
        }

        topic_prefix_ = "/" + agent_name_ + "/input/";

        // 创建互斥回调组，保证对 sources_ 的所有操作串行化
        callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

        // 创建快照服务
        snapshot_srv_ = create_service<GetSnapshot>(
            "/" + agent_name_ + "/input",
            std::bind(&InputMgmtNode::handle_snapshot, this,
                      std::placeholders::_1, std::placeholders::_2),
            rmw_qos_profile_services_default, callback_group_);

        // 定时发现新输入源
        discovery_timer_ = create_wall_timer(
            std::chrono::duration<double>(discovery_period),
            std::bind(&InputMgmtNode::discover_inputs, this), callback_group_);

        // 定时清理超时输入源
        cleanup_timer_ = create_wall_timer(
            CLEANUP_INTERVAL,
            std::bind(&InputMgmtNode::cleanup_inputs, this), callback_group_);

        RCLCPP_INFO(get_logger(), "InputMgmtNode started for agent %s", agent_name_.c_str());
    }

private:
    // ----------------------------------------------------------------
    // 发现新输入源
    // ----------------------------------------------------------------
    void discover_inputs() {
        try {
            auto topics_and_types = get_topic_names_and_types();
            std::regex info_regex(topic_prefix_ + "([^/]+)/info");
            std::smatch match;

            for (const auto& [topic, types] : topics_and_types) {
                if (std::regex_match(topic, match, info_regex)) {
                    std::string src_name = match[1].str();
                    // 跳过已注册的源
                    if (sources_.count(src_name)) continue;
                    // 跳过无活跃发布者的残留话题
                    if (count_publishers(topic) == 0) continue;

                    auto entry = std::make_shared<InputEntry>();
                    entry->name = src_name;
                    entry->last_info_time = now(); // 初始时间戳，避免立即被超时清理

                    rclcpp::SubscriptionOptions opts;
                    opts.callback_group = callback_group_;

                    // 订阅 info 话题（JSON 字符串）
                    entry->info_sub = create_subscription<std_msgs::msg::String>(
                        topic_prefix_ + src_name + "/info",
                        rclcpp::QoS(10).transient_local().reliable(),
                        [this, src_name](const std_msgs::msg::String::SharedPtr msg) {
                            auto it = sources_.find(src_name);
                            if (it == sources_.end()) return;
                            try {
                                json j = json::parse(msg->data);
                                it->second->desc = j.value("desc", "");
                                it->second->mode = j.value("mode", "accumulate");
                                if (it->second->mode.empty()) it->second->mode = "accumulate";
                                it->second->status = j.value("status", "ok");
                                it->second->last_info_time = now();
                            } catch (const json::parse_error&) {
                                RCLCPP_WARN(get_logger(), "Invalid JSON from %s info", src_name.c_str());
                                // 忽略此次更新，该源可能因超时被移除
                            }
                        }, opts);

                    // 订阅 data 话题
                    entry->data_sub = create_subscription<std_msgs::msg::String>(
                        topic_prefix_ + src_name,
                        rclcpp::QoS(10).reliable(),
                        [this, src_name](const std_msgs::msg::String::SharedPtr msg) {
                            auto it = sources_.find(src_name);
                            if (it == sources_.end()) return;
                            try {
                                if (it->second->mode == "latest") {
                                    it->second->pending_data.clear();
                                }
                                it->second->pending_data.push_back(msg->data);
                            } catch (...) {
                                RCLCPP_ERROR(get_logger(), "Error handling data from %s", src_name.c_str());
                            }
                        }, opts);

                    sources_[src_name] = entry;
                    RCLCPP_INFO(get_logger(), "Discovered input source: %s", src_name.c_str());
                }
            }
        } catch (const std::exception& e) {
            RCLCPP_ERROR(get_logger(), "discover_inputs exception: %s", e.what());
        }
    }

    // ----------------------------------------------------------------
    // 清理超时或失联的输入源
    // ----------------------------------------------------------------
    void cleanup_inputs() {
        try {
            auto now_time = now();
            std::vector<std::string> to_remove;
            for (const auto& [name, entry] : sources_) {
                std::string info_topic = topic_prefix_ + name + "/info";
                // 立即移除无发布者的源
                if (count_publishers(info_topic) == 0) {
                    RCLCPP_WARN(get_logger(), "Source %s publisher gone", name.c_str());
                    to_remove.push_back(name);
                    continue;
                }
                // 移除心跳超时的源
                if ((now_time - entry->last_info_time).seconds() > info_timeout_) {
                    RCLCPP_WARN(get_logger(), "Source %s heartbeat timeout", name.c_str());
                    to_remove.push_back(name);
                }
            }
            for (const auto& name : to_remove) {
                sources_.erase(name);
            }
        } catch (const std::exception& e) {
            RCLCPP_ERROR(get_logger(), "cleanup_inputs exception: %s", e.what());
        }
    }

    // ----------------------------------------------------------------
    // 快照服务处理
    // ----------------------------------------------------------------
    void handle_snapshot(
        const std::shared_ptr<GetSnapshot::Request>,
        std::shared_ptr<GetSnapshot::Response> res)
    {
        // 先清理超时源，避免已失联源的数据返回
        cleanup_inputs();

        json snapshot = json::object();
        for (auto& [name, entry] : sources_) {
            if (entry->pending_data.empty()) continue;  // 无新数据则跳过该源

            // 将累积数据数组赋值给 JSON 对象
            snapshot[name] = entry->pending_data;
            // 清空该源缓存，准备下一轮累积
            entry->pending_data.clear();
        }

        // 将 JSON 对象序列化为字符串返回
        res->snapshot_json = snapshot.dump();
    }

    // 成员变量
    std::string agent_name_;
    std::string topic_prefix_;
    double info_timeout_;

    rclcpp::CallbackGroup::SharedPtr callback_group_;
    rclcpp::Service<GetSnapshot>::SharedPtr snapshot_srv_;
    rclcpp::TimerBase::SharedPtr discovery_timer_;
    rclcpp::TimerBase::SharedPtr cleanup_timer_;

    // 输入源表：键为源名称，值为 InputEntry 智能指针
    std::unordered_map<std::string, std::shared_ptr<InputEntry>> sources_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);

    // 临时节点获取 agent_name 参数
    auto temp = std::make_shared<rclcpp::Node>("temp");
    temp->declare_parameter<std::string>("agent_name", "agent");
    std::string agent_name = temp->get_parameter("agent_name").as_string();
    temp.reset();

    auto node = std::make_shared<InputMgmtNode>(agent_name);
    rclcpp::executors::MultiThreadedExecutor executor;
    executor.add_node(node);
    executor.spin();

    rclcpp::shutdown();
    return 0;
}