// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// ================================================================
// Cloud-Soul 统一输出管理节点
// ================================================================
//
// 作用:
//   作为 Agent 的输出代理层，统一管理所有子工具。负责工具发现、
//   心跳维护、Goal 路由、超时控制和取消转发。对上层暴露单一 Action
//   接口，对下层（子工具）透明代理。
//
// 节点名: /<agent_name>/output_mgmt_node
//
// 参数:
//   agent_name             (string, 必填)  Agent 命名空间
//   info_timeout           (double, 3.0)   子工具 info 心跳超时秒数
//   discovery_period       (double, 1.0)   工具发现扫描周期秒数
//   default_timeout        (double, 30.0)  子工具默认执行超时秒数
//   cancel_timeout         (double, 2.0)   取消等待超时秒数
//   delay_timeout          (double, 5.0)   管理超时延迟缓冲秒数
//   action_server_timeout  (double, 1.0)   等待 Action Server 就绪超时秒数
//
// Server:
//   /<agent_name>/output/info  (GetToolsInfo)
//     返回当前在线工具的 description_json 数组，供 LLM function-calling 使用
//
// Action:
//   /<agent_name>/output  (ExecuteTool)
//     Goal:   接收 {name, arguments} → 路由到对应子工具 → 透传结果
//     Cancel: 转发取消给正在执行的子工具
//
// 子工具设计规范:
//   每个子工具是一个独立的 ROS2 Action Server，需满足：
//   1. Action 类型: cs_interfaces/action/ExecuteTool
//   2. Action 名:   /<agent_name>/output/<tool_name>
//   3. Info 话题:  /<agent_name>/output/<tool_name>/info (std_msgs/String)
//      发布 DeepSeek/OpenAI function-calling 格式的工具描述 JSON，
//      同时作为心跳（频率任意，超时由 info_timeout 参数控制）
//   4. Goal input_json 格式:
//       {"name": "<tool_name>", "arguments": {子工具实际参数}}
//       （与 output_mgmt 收到的格式完全一致，不剥壳）
//   5. Result output_json: 自由字符串，将原样透传给 LLM
//
// 上层传入 JSON 规范 (来自 agent_loop):
//   agent_loop 将 LLM tool_calls 转为如下格式发送给 /<agent_name>/output:
//   {
//     "name": "shell_exec",           // 工具名，来自 function.name
//     "arguments": {                  // 工具参数，来自 function.arguments (已 parse)
//       "command": "ls -la",
//       "timeout_sec": 10             // timeout 是必须参数
//     }
//   }
//   注意: arguments 中的 timeout_sec 会被 output_mgmt 复制用于 deadline 计算
//
// 关键设计:
//   - output_mgmt 与子工具接收相同 JSON 结构（不剥壳）
//   - 同一时间只允许一个工具执行（新 Goal 会杀旧）
//   - 子工具执行超时先尝试取消，取消失败则报错
//

// output_mgmt_node_pre.cpp

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "cs_interfaces/srv/get_tools_info.hpp"
#include "nlohmann/json.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GetToolsInfo = cs_interfaces::srv::GetToolsInfo;
using json = nlohmann::json;

// 轮询间隔：等待 Goal 发送/结果时的睡眠周期
static constexpr auto GOAL_POLL_INTERVAL = std::chrono::milliseconds(100);

// ================================================================
// OutputMgmtNode
// ================================================================
class OutputMgmtNode : public rclcpp::Node {
public:
    OutputMgmtNode(const std::string& agent_name)
        : Node("output_mgmt_node", agent_name), agent_name_(agent_name)
    {
        // 参数声明
        declare_parameter("agent_name", agent_name); // agent 名/命名空间
        declare_parameter("info_timeout", 3.0); // 子工具的超时心跳
        declare_parameter("discovery_period", 1.0); // 扫描子工具的频率
        declare_parameter("default_timeout", 30.0); // 默认工具超时时间，如果工具超时将会触发取消
        declare_parameter("cancel_timeout", 2.0); // 取消等待时间
        declare_parameter("delay_timeout", 5.0); // 由于 default_timeout 与子工具的 default_timeout 一致，这里必须延迟一段时间，再报 mgmt timeout
        declare_parameter("action_server_timeout", 1.0); // 等待子工具 Action Server 就绪的超时秒数

        // 工具列表查询服务
        info_srv_ = create_service<GetToolsInfo>(
            "/" + agent_name_ + "/output/info",
            [this](const std::shared_ptr<GetToolsInfo::Request> req, std::shared_ptr<GetToolsInfo::Response> res) { handle_info_request(req, res); });

        // 统一 Action Server
        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this, "/" + agent_name_ + "/output",
            [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
            [this](auto gh) { return handle_cancel(gh); },  // 客户端请求取消 Goal 时触发
            [this](auto gh) { handle_accepted(gh); });  // Goal 被接受后触发，启动执行线程

        // 工具发现定时器
        discovery_timer_ = create_wall_timer(
            std::chrono::duration<double>(get_parameter("discovery_period").as_double()),
            [this]() { handle_tool_heartbeat(); });

        RCLCPP_INFO(get_logger(), "output_mgmt_node started");
    }

private:
    // ---------- 工具信息 ----------
    struct ToolInfo {
        std::string name;
        std::string description_json;
        rclcpp::Time last_seen;
        rclcpp_action::Client<ExecuteTool>::SharedPtr action_client;
        rclcpp::Subscription<std_msgs::msg::String>::SharedPtr info_sub;
        std::atomic<bool> busy{false};
        // 子 Goal handle，用于强制终止正在执行的任务
        std::shared_ptr<rclcpp_action::ClientGoalHandle<ExecuteTool>> sub_goal_handle;

    };

    // ---------- 查询工具列表 ----------
    void handle_info_request(
        const std::shared_ptr<GetToolsInfo::Request>,
        std::shared_ptr<GetToolsInfo::Response> response)
    {
        json tools_arr = json::array();
        for (const auto& [name, info] : tools_) {
            try {
                tools_arr.push_back(json::parse(info->description_json));
            } catch (...) {}
        }
        response->tools_json = tools_arr.dump();
    }

    // ---------- 工具发现 ----------
    void handle_tool_heartbeat() {
        // 获取 ROS2 图中所有活跃话题名和类型
        auto topic_names = get_topic_names_and_types();
        std::string prefix = "/" + agent_name_ + "/output/";
        std::string info_suffix = "/info";

        // 遍历
        for (const auto& [topic_name, types] : topic_names) {
            // 过滤 info 类型：必须是 std_msgs/msg/String
            bool is_string_type = false;
            for (const auto& t : types) {
                if (t == "std_msgs/msg/String") { is_string_type = true; break; }
            }
            if (!is_string_type) continue;

            // 过滤 /agent_name/output/ 头
            if (topic_name.find(prefix) != 0) continue;

            // 过滤 /info 尾
            if (topic_name.find(info_suffix) == std::string::npos) continue;

            // 过滤空工具名
            if (topic_name.size() <= prefix.size() + info_suffix.size()) continue;

            // 构造工具名（掐头去尾）
            // /agent/output/{tool_name}/info → tool_name
            size_t tool_start = prefix.size();
            size_t tool_end = topic_name.find(info_suffix);
            std::string tool_name = topic_name.substr(tool_start, tool_end - tool_start);

            // 检查是否已注册，已存在则跳过
            if (tools_.find(tool_name) != tools_.end()) continue;

            // 初始化新 ToolInfo
            auto tool = std::make_shared<ToolInfo>();
            tool->name = tool_name;
            tool->last_seen = now();

            // 订阅该工具的 info 话题，接收心跳和工具描述 JSON
            tool->info_sub = create_subscription<std_msgs::msg::String>(
                topic_name, rclcpp::QoS(1).reliable().transient_local(),
                [this, tool_name](const std_msgs::msg::String::SharedPtr msg) {
                    auto it = tools_.find(tool_name);
                    if (it != tools_.end()) {
                        it->second->description_json = msg->data;  // 更新工具描述
                        it->second->last_seen = now();              // 刷新心跳时间
                    }
                });

            // 创建到该工具 Action Server 的客户端
            std::string action_name = "/" + agent_name_ + "/output/" + tool_name;
            tool->action_client = rclcpp_action::create_client<ExecuteTool>(this, action_name);

            // 注册到工具表
            tools_[tool_name] = tool;
            RCLCPP_INFO(get_logger(), "Tool discovered: %s", tool_name.c_str());
        }

        // 心跳超时移除：last_seen 超过 info_timeout 的工具从表中删除
        auto now_time = now();
        double info_timeout = get_parameter("info_timeout").as_double();
        std::vector<std::string> to_remove;
        for (const auto& [name, info] : tools_) {
            if ((now_time - info->last_seen).seconds() > info_timeout) {
                to_remove.push_back(name);
            }
        }
        for (const auto& name : to_remove) {
            tools_.erase(name);
        // TODO: 热插拔健壮性 — 工具节点崩溃后, output_mgmt 的 action client 和 subscription
        // 可能变成僵尸状态, 导致其他正常工具也出现 "Tool not found" 错误。
        //
        // 问题背景: 2026-07-02 手动 kill web_fetch_node 后重启, output_mgmt 的 tools_ map
        // 仍持有旧节点的 stale action client, 心跳扫描因僵尸连接超时而短暂瘫痪整个工具发现机制。
        //
        // 现象: 所有工具节点 (shell_exec/web_fetch/message_send 等) 连续返回 "Tool not found"
        // 或 "cannot stop tool", 即使节点实际在线。需要完整重启 Adam 才能恢复。
        //
        // 预计方案:
        //   1. 在 handle_tool_heartbeat() 中检测 action_client 是否 server 已不可达,
        //      若不可达则主动清理 (tools_.erase + reset action_client)
        //   2. 或在调用 action client 时加超时 + 失败自动重试, 避免僵尸连接阻塞
        //   3. 工具发现时用 wait_for_action_server() 验证可达性, 而非只依赖 topic 存在

            RCLCPP_WARN(get_logger(), "Tool removed due to heartbeat timeout: %s", name.c_str());
        }
    }

    // ---------- 处理 Goal 接受 ----------
    void handle_accepted(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        std::thread{[this, goal_handle]() { handle_goal(goal_handle); }}.detach();
    }

    // ---------- 核心：处理 Goal ----------
    void handle_goal(const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        auto result = std::make_shared<ExecuteTool::Result>();
        const auto goal = goal_handle->get_goal();

        // 强制终止正在执行的任务（同一时间只允许一个工具运行）
        {
            std::lock_guard<std::mutex> lock(tools_mutex_);
            for (auto& [n, info] : tools_) {
                if (info->busy.load() && info->sub_goal_handle) {
                    RCLCPP_WARN(get_logger(), "handle_goal: Killing running tool: %s", n.c_str());
                    info->action_client->async_cancel_goal(info->sub_goal_handle);
                    info->sub_goal_handle.reset();
                    info->busy.store(false);
                }
            }
        }

        // 解析 input_json: {"name": "shell_exec", "arguments": {...}}
        json goal_json;
        try {
            goal_json = json::parse(goal->input_json);
        } catch (...) {
            RCLCPP_ERROR(get_logger(), "handle_goal: Invalid input JSON");
            result->output_json = "[output_mgmt] Invalid input JSON";
            goal_handle->abort(result);
            return;
        }

        // 提取 timeout_sec，没有则用 default
        double timeout = 0.0;
        if (goal_json.contains("arguments") && goal_json["arguments"].contains("timeout_sec")) {
            timeout = goal_json["arguments"]["timeout_sec"].get<double>();
        }
        if (timeout <= 0.0) {
            timeout = get_parameter("default_timeout").as_double();
        }
        double delay = get_parameter("delay_timeout").as_double();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout + delay);

        // 从 json 中查找工具名
        if (!goal_json.contains("name") || !goal_json["name"].is_string()) {
            RCLCPP_ERROR(get_logger(), "handle_goal: Missing tool name");
            result->output_json = "[output_mgmt] Missing tool name";
            goal_handle->abort(result);
            return;
        }
        std::string tool_name = goal_json["name"].get<std::string>();

        // 查看工具是否可用
        std::shared_ptr<ToolInfo> tool;
        {
            std::lock_guard<std::mutex> lock(tools_mutex_);
            auto it = tools_.find(tool_name);
            if (it == tools_.end()) {
                RCLCPP_ERROR(get_logger(), "handle_goal: Tool not found: %s", tool_name.c_str());
                result->output_json = "[output_mgmt] Tool not found: " + tool_name;
                goal_handle->abort(result);
                return;
            }
            tool = it->second;
            // 防御性检查：理论上 kill 逻辑已清掉 busy，但保留防止竞态（子工具重构后会修复 JSON 路径）
            if (tool->busy.exchange(true)) {
                RCLCPP_WARN(get_logger(), "handle_goal: Tool is busy: %s", tool_name.c_str());
                result->output_json = "[output_mgmt] Tool is busy: " + tool_name;
                goal_handle->abort(result);
                return;
            }
        }

        // 透传原始 input_json 给子工具
        auto sub_goal = ExecuteTool::Goal();
        sub_goal.input_json = goal->input_json;

        // 检查子工具 Action Server
        double action_server_timeout = get_parameter("action_server_timeout").as_double();
        if (!tool->action_client->wait_for_action_server(std::chrono::duration<double>(action_server_timeout))) {
            RCLCPP_ERROR(get_logger(), "handle_goal: Tool connection lost: %s", tool_name.c_str());
            result->output_json = "[output_mgmt] Tool connection lost: " + tool_name;
            goal_handle->abort(result);
            tool->busy.store(false);
            return;
        }

        // 发送 Goal
        auto send_future = tool->action_client->async_send_goal(sub_goal);

        // 轮询等待 send 就绪或超时
        while (std::chrono::steady_clock::now() < deadline &&
               send_future.wait_for(GOAL_POLL_INTERVAL) != std::future_status::ready) {}

        // send 阶段超时
        if (std::chrono::steady_clock::now() >= deadline) {
            RCLCPP_ERROR(get_logger(), "handle_goal: Send timeout: %s", tool_name.c_str());
            result->output_json = "[output_mgmt] Send timeout: " + tool_name;
            goal_handle->abort(result);
            tool->busy.store(false);
            return;
        }

        // send 出现意外
        auto sub_goal_handle = send_future.get();
        tool->sub_goal_handle = sub_goal_handle;
        if (!sub_goal_handle) {
            RCLCPP_ERROR(get_logger(), "handle_goal: Tool disconnected during send: %s", tool_name.c_str());
            result->output_json = "[output_mgmt] Tool disconnected during send: " + tool_name;
            goal_handle->abort(result);
            tool->busy.store(false);
            return;
        }

        // 轮询等待工具执行结果或超时
        auto result_future = tool->action_client->async_get_result(sub_goal_handle);
        while (std::chrono::steady_clock::now() < deadline &&
               result_future.wait_for(GOAL_POLL_INTERVAL) != std::future_status::ready) {}

        // 执行超时
        if (std::chrono::steady_clock::now() >= deadline) {
            RCLCPP_WARN(get_logger(), "handle_goal: Tool execution timeout: %s", tool_name.c_str());
            bool cancelled = cancel_sub_tool(tool, sub_goal_handle);
            if (cancelled) {
                RCLCPP_INFO(get_logger(), "handle_goal: Tool cancelled after timeout: %s", tool_name.c_str());
                result->output_json = "[output_mgmt] Timeout, tool cancelled";
            } else {
                RCLCPP_ERROR(get_logger(), "handle_goal: Failed to cancel tool: %s", tool_name.c_str());
                result->output_json = "[output_mgmt] Error, cannot stop tool";
            }
            goal_handle->abort(result);
            tool->sub_goal_handle.reset();
            tool->busy.store(false);
            return;
        }

        // 执行成功，透传内容
        auto wrapped = result_future.get();
        result->output_json = wrapped.result->output_json;
        RCLCPP_INFO(get_logger(), "handle_goal: Tool succeeded: %s", tool_name.c_str());
        goal_handle->succeed(result);
        tool->sub_goal_handle.reset();
        tool->busy.store(false);
    }


    // ---------- 取消子工具 ----------
    // 取消子工具，返回 true 表示取消成功（子工具已响应）
    bool cancel_sub_tool(
        std::shared_ptr<ToolInfo> tool,
        std::shared_ptr<rclcpp_action::ClientGoalHandle<ExecuteTool>> sub_goal)
    {
        tool->action_client->async_cancel_goal(sub_goal);
        double cancel_timeout = get_parameter("cancel_timeout").as_double();
        auto cancel_deadline = std::chrono::steady_clock::now() +
            std::chrono::duration<double>(cancel_timeout);

        auto result_future = tool->action_client->async_get_result(sub_goal);
        while (std::chrono::steady_clock::now() < cancel_deadline &&
               result_future.wait_for(GOAL_POLL_INTERVAL) != std::future_status::ready) {}

        return std::chrono::steady_clock::now() < cancel_deadline;
    }

    // ---------- 处理取消 ----------
    rclcpp_action::CancelResponse handle_cancel(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        for (auto& [name, info] : tools_) {
            if (info->busy.load() && info->sub_goal_handle) {
                RCLCPP_INFO(get_logger(), "handle_cancel: Cancelling tool: %s", name.c_str());
                if (!cancel_sub_tool(info, info->sub_goal_handle)) {
                    RCLCPP_ERROR(get_logger(), "handle_cancel: Failed to cancel tool: %s", name.c_str());
                    return rclcpp_action::CancelResponse::REJECT;
                }
            }
        }
        return rclcpp_action::CancelResponse::ACCEPT;
    }

    // ========== 成员变量 ==========
    std::string agent_name_;
    rclcpp::Service<GetToolsInfo>::SharedPtr info_srv_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
    rclcpp::TimerBase::SharedPtr discovery_timer_;
    std::mutex tools_mutex_;
    std::map<std::string, std::shared_ptr<ToolInfo>> tools_;
};

// ================================================================
// main
// ================================================================
int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto temp = std::make_shared<rclcpp::Node>("temp");
    temp->declare_parameter<std::string>("agent_name", "agent");
    std::string agent_name = temp->get_parameter("agent_name").as_string();
    temp.reset();
    auto node = std::make_shared<OutputMgmtNode>(agent_name);
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
