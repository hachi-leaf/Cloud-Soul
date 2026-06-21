// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: <agent_name>_output_mgmt
// 作用: 自动发现并管理 /agent_name/output/* 工具节点，提供聚合工具列表和统一调用入口。
//
// 参数:
//   agent_name       - 命名空间前缀，默认 "agent"
//   tool_timeout     - 工具执行超时 (秒)，默认 10.0
//   info_timeout     - 工具信息心跳超时 (秒)，默认 3.0
//   discovery_period - 工具发现扫描周期 (秒)，默认 1.0
//
// 发布/订阅:
//   订阅 /<agent_name>/output/+/info  (std_msgs/String) 工具描述 JSON，QoS transient_local
//   服务 /<agent_name>/output/info (GetToolsInfo) 返回所有在线工具的 OpenAI tools JSON
//   动作 /<agent_name>/output (ExecuteTool) 统一工具调用入口
//
// Note: 工具通过 topic 发现，心跳超时后自动移除；动作执行在多线程中处理。

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"

#include "cs_interfaces/srv/get_tools_info.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using namespace std::chrono_literals;
using GetToolsInfo = cs_interfaces::srv::GetToolsInfo;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

// -----------------------------------------------------------------------------
// 单个工具的信息维护
// -----------------------------------------------------------------------------
struct ToolEntry {
  std::string name;                          // 工具名（从话题中提取）
  std::string info_json;                     // 最近一次 info 话题的原始 JSON
  rclcpp::Time last_update;                  // 最后收到 info 的时间
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr info_sub; // 订阅句柄
  rclcpp_action::Client<ExecuteTool>::SharedPtr action_client;     // 动作客户端

  ToolEntry() : last_update(0, 0, RCL_ROS_TIME) {}
};

// -----------------------------------------------------------------------------
// 核心管理节点
// -----------------------------------------------------------------------------
class AgentOutputMgmt : public rclcpp::Node {
public:
  AgentOutputMgmt(const std::string & node_name, const std::string & agent_name)
  : Node(node_name), agent_name_(agent_name)
  {
    // 参数声明与获取
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("tool_timeout", 10.0);
    this->declare_parameter<double>("info_timeout", 3.0);
    this->declare_parameter<double>("discovery_period", 1.0);

    tool_timeout_ = this->get_parameter("tool_timeout").as_double();
    info_timeout_ = this->get_parameter("info_timeout").as_double();
    double discovery_period = this->get_parameter("discovery_period").as_double();

    // 构建话题前缀，例如 "/agent_name/output/"
    topic_prefix_ = "/" + agent_name_ + "/output/";

    // 服务：返回工具列表
    get_tools_srv_ = this->create_service<GetToolsInfo>(
      topic_prefix_ + "info",
      std::bind(&AgentOutputMgmt::handle_get_tools, this, std::placeholders::_1, std::placeholders::_2));

    // 动作服务器：对外提供统一的工具调用入口
    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      topic_prefix_.substr(0, topic_prefix_.size() - 1),  // 去掉末尾 '/'，动作名 "/agent_name/output"
      std::bind(&AgentOutputMgmt::handle_goal, this, std::placeholders::_1, std::placeholders::_2),
      std::bind(&AgentOutputMgmt::handle_cancel, this, std::placeholders::_1),
      std::bind(&AgentOutputMgmt::handle_accepted, this, std::placeholders::_1));

    // 周期性扫描新工具话题
    discovery_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(discovery_period),
      std::bind(&AgentOutputMgmt::discover_tools, this));

    // 周期性清理超时工具
    cleanup_timer_ = this->create_wall_timer(
      2s, std::bind(&AgentOutputMgmt::cleanup_tools, this));

    RCLCPP_INFO(this->get_logger(), "AgentOutputMgmt node started, agent: %s", agent_name_.c_str());
  }

private:
  // ---------------------------------------------------------------------------
  // 工具发现：通过 topic list 找出所有 /agent_name/output/+/info 话题
  // ---------------------------------------------------------------------------
    void discover_tools() {
        auto topics_and_types = this->get_topic_names_and_types();
        std::regex info_regex(topic_prefix_ + "([^/]+)/info");
        std::smatch match;

        for (const auto & [topic, types] : topics_and_types) {
        if (std::regex_match(topic, match, info_regex)) {
            std::string tool_name = match[1].str();
            if (tools_.find(tool_name) != tools_.end()) {
            continue;
            }
            RCLCPP_INFO(this->get_logger(), "发现新工具: %s", tool_name.c_str());

            auto entry = std::make_shared<ToolEntry>();
            entry->name = tool_name;
            // 初始化为当前时间，避免心跳检查立即超时
            entry->last_update = this->now();

            // 订阅 info 话题
            entry->info_sub = this->create_subscription<std_msgs::msg::String>(
            topic_prefix_ + tool_name + "/info",
            rclcpp::QoS(1).transient_local().reliable(),
            [this, tool_name](const std_msgs::msg::String::SharedPtr msg) {
                auto it = tools_.find(tool_name);
                if (it != tools_.end()) {
                it->second->info_json = msg->data;
                it->second->last_update = this->now();
                }
            });

            entry->action_client = rclcpp_action::create_client<ExecuteTool>(
            this, topic_prefix_ + tool_name);

            tools_[tool_name] = entry;
        }
        }
    }

  // ---------------------------------------------------------------------------
  // 超时清理：移除长时间未发布 info 的工具
  // ---------------------------------------------------------------------------
  void cleanup_tools() {
    auto now = this->now();
    std::vector<std::string> to_remove;
    for (const auto & [name, entry] : tools_) {
      double age = (now - entry->last_update).seconds();
      if (age > info_timeout_) {
        to_remove.push_back(name);
      }
    }
    for (const auto & name : to_remove) {
      RCLCPP_WARN(this->get_logger(), "工具 %s 心跳超时，已移除", name.c_str());
      tools_.erase(name);
    }
  }

  // ---------------------------------------------------------------------------
  // 服务回调：收集所有可用工具的 OpenAI 格式描述
  // ---------------------------------------------------------------------------
  void handle_get_tools(
    const std::shared_ptr<GetToolsInfo::Request> /*req*/,
    std::shared_ptr<GetToolsInfo::Response> res)
  {
    // 先做一次清理，确保列表新鲜
    cleanup_tools();

    std::string tools_json = "[";
    bool first = true;
    for (const auto & [name, entry] : tools_) {
      if (entry->info_json.empty()) {
        continue;  // 还没收到 info 的工具暂时不加入
      }
      if (!first) tools_json += ",";
      first = false;
      // 将工具描述的 function 对象包装为 OpenAI 格式
      // entry->info_json 应该形如 {"name":"...","description":"...","parameters":...}
      tools_json += R"({"type":"function","function":)" + entry->info_json + "}";
    }
    tools_json += "]";
    res->tools_json = tools_json;
  }

  // ---------------------------------------------------------------------------
  // 动作服务器：goal 处理
  // ---------------------------------------------------------------------------
  rclcpp_action::GoalResponse handle_goal(
    const rclcpp_action::GoalUUID & /*uuid*/,
    std::shared_ptr<const ExecuteTool::Goal> goal)
  {
    (void)goal;
    return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
  }

  rclcpp_action::CancelResponse handle_cancel(
    const std::shared_ptr<GoalHandleExecute> /*goal_handle*/)
  {
    return rclcpp_action::CancelResponse::ACCEPT;
  }

  void handle_accepted(const std::shared_ptr<GoalHandleExecute> goal_handle) {
    // 将实际执行放到独立线程以避免阻塞执行器，同时保留超时能力
    std::thread{std::bind(&AgentOutputMgmt::execute, this, goal_handle)}.detach();
  }

  // ---------------------------------------------------------------------------
  // 实际执行：解析输入、调用具体工具、返回结果
  // ---------------------------------------------------------------------------
  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle) {
    auto result = std::make_shared<ExecuteTool::Result>();
    const auto goal = goal_handle->get_goal();
    const std::string & input_json = goal->input_json;

    // 1. 解析 input_json，获取工具名和参数
    std::string tool_name;
    std::string arguments_json;
    try {
      // 简单的 JSON 解析：只提取 "name" 和 "arguments" 字段
      // 这里使用手动解析，避免引入第三方 JSON 库。假设格式标准。
      tool_name = extract_json_string(input_json, "name");
      arguments_json = extract_json_object(input_json, "arguments");
      if (tool_name.empty()) {
        throw std::runtime_error("missing 'name' field");
      }
    } catch (const std::exception & e) {
      RCLCPP_ERROR(this->get_logger(), "解析 input_json 失败: %s", e.what());
      result->output_json = R"({"error":"invalid input json format"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    // 2. 查找工具，检查可用性
    auto it = tools_.find(tool_name);
    if (it == tools_.end()) {
      result->output_json = R"({"error":"tool not found"})";
      result->exit_code = -2;
      goal_handle->abort(result);
      return;
    }

    auto & entry = it->second;
    if (!entry->action_client) {
      result->output_json = R"({"error":"tool action client not ready"})";
      result->exit_code = -3;
      goal_handle->abort(result);
      return;
    }

    // 等待工具动作服务器上线（短超时）
    if (!entry->action_client->wait_for_action_server(1s)) {
      result->output_json = R"({"error":"tool action server not available"})";
      result->exit_code = -4;
      goal_handle->abort(result);
      return;
    }

    // 3. 构建目标并发送给工具节点
    auto tool_goal = ExecuteTool::Goal();
    tool_goal.input_json = arguments_json;

    auto send_goal_future = entry->action_client->async_send_goal(tool_goal);
    // 等待 goal 被服务器接受（短超时）
    if (send_goal_future.wait_for(1s) != std::future_status::ready) {
      result->output_json = R"({"error":"tool did not accept goal"})";
      result->exit_code = -5;
      goal_handle->abort(result);
      return;
    }

    auto goal_handle_tool = send_goal_future.get();
    if (!goal_handle_tool) {
      result->output_json = R"({"error":"tool rejected goal"})";
      result->exit_code = -6;
      goal_handle->abort(result);
      return;
    }

    // 4. 等待结果，带超时
    auto result_future = entry->action_client->async_get_result(goal_handle_tool);
    auto timeout_duration = std::chrono::duration<double>(tool_timeout_);
    if (result_future.wait_for(timeout_duration) != std::future_status::ready) {
      // 超时，取消工具端的 goal
      entry->action_client->async_cancel_goal(goal_handle_tool);
      result->output_json = R"({"error":"tool execution timeout"})";
      result->exit_code = -7;
      goal_handle->abort(result);
      return;
    }

    // 5. 获取结果
    auto wrapped_result = result_future.get();
    if (wrapped_result.code != rclcpp_action::ResultCode::SUCCEEDED) {
      // 修正：使用自定义定界符避免括号冲突
      result->output_json = R"EOF({"error":"tool execution failed (action aborted)"})EOF";
      result->exit_code = -8;
      goal_handle->abort(result);
      return;
    }

    // 6. 透传工具返回的结果
    result->output_json = wrapped_result.result->output_json;
    result->exit_code = wrapped_result.result->exit_code;  // 可能为 0 或工具的业务错误码
    goal_handle->succeed(result);
  }

  // ---------------------------------------------------------------------------
  // 轻量 JSON 字段提取工具（仅用于解析 input_json）
  // ---------------------------------------------------------------------------
  static std::string extract_json_string(const std::string & json, const std::string & key) {
    // 搜索 "\"key\":\"value\"" 模式的字符串值
    std::string search = "\"" + key + "\":\"";
    size_t start = json.find(search);
    if (start == std::string::npos) return "";
    start += search.length();
    size_t end = start;
    while (end < json.size()) {
      if (json[end] == '"' && (end == 0 || json[end-1] != '\\')) {
        break;
      }
      ++end;
    }
    return json.substr(start, end - start);
  }

  static std::string extract_json_object(const std::string & json, const std::string & key) {
    // 搜索 "\"key\":{" 到匹配的 "}"
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start == std::string::npos) return "{}";
    start += search.length();
    // 跳过空白
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\n')) ++start;
    if (start >= json.size() || json[start] != '{') return "{}";
    int brace_count = 1;
    size_t end = start + 1;
    while (end < json.size() && brace_count > 0) {
      if (json[end] == '{') ++brace_count;
      else if (json[end] == '}') --brace_count;
      ++end;
    }
    return json.substr(start, end - start);
  }

  // ========== 成员变量声明 ==========
  std::string agent_name_;
  std::string topic_prefix_;
  double tool_timeout_;
  double info_timeout_;

  rclcpp::Service<GetToolsInfo>::SharedPtr get_tools_srv_;
  rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;

  rclcpp::TimerBase::SharedPtr discovery_timer_;
  rclcpp::TimerBase::SharedPtr cleanup_timer_;

  std::unordered_map<std::string, std::shared_ptr<ToolEntry>> tools_;
};

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);

  // 从参数获取 agent_name，如果未指定则使用默认值 "agent"
  auto temp_node = std::make_shared<rclcpp::Node>("temp");
  temp_node->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp_node->get_parameter("agent_name").as_string();
  temp_node.reset();

  // 构造节点名称：{agent_name}_output_mgmt
  std::string node_name = agent_name + "_output_mgmt";
  auto node = std::make_shared<AgentOutputMgmt>(node_name, agent_name);

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}