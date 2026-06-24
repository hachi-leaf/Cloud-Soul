// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: /<agent_name>/output_mgmt_node
// 作用: 自动发现并管理 /<agent_name>/output/* 工具节点，提供聚合工具列表和统一调用入口。
//       支持 input_json 中的多余空白（节点会自动压缩）。
//       支持取消转发：当管理节点动作被取消时，会取消工具端动作并返回取消信息。
//       工具 abort 时透传工具的原始错误信息。
//
// 参数:
//   agent_name       - 命名空间，默认 "agent"
//   tool_timeout     - 工具执行默认超时(秒)，可在每次调用中覆盖，默认 60.0
//   info_timeout     - 工具心跳超时(秒)，默认 3.0
//   discovery_period - 话题扫描周期(秒)，默认 1.0
//
// 对外接口:
//   服务   /<agent_name>/output/info      (GetToolsInfo)  返回在线工具的 OpenAI tools 列表
//   动作   /<agent_name>/output           (ExecuteTool)   统一工具调用入口，goal 可携带 timeout_sec
//
// 工具节点开发要求:
//   1. 发布自身描述: 话题 /<agent_name>/output/<tool_name>/info, 消息类型 std_msgs/String
//      QoS: transient_local, reliable. 内容为 OpenAI function 的 JSON (不含外层 type):
//        {"name":"...","description":"...","parameters":{...}}
//      需周期性发布 (周期 < info_timeout) 以维持心跳。
//   2. 提供动作服务器: /<agent_name>/output/<tool_name>, 类型 ExecuteTool (cs_interfaces)
//      Goal: input_json 为调用参数字符串 (即 function arguments 的 JSON)。
//      Result: output_json 为执行结果字符串, exit_code 为自定义业务码 (0 正常)。

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <map>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"

#include "cs_interfaces/srv/get_tools_info.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "cs_interfaces/constants.hpp"

using namespace std::chrono_literals;
using GetToolsInfo = cs_interfaces::srv::GetToolsInfo;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

// ------------------------------------------------------------
// 工具条目
// ------------------------------------------------------------
struct ToolEntry {
  std::string name;
  std::string info_json;
  rclcpp::Time last_update;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr info_sub;
  rclcpp_action::Client<ExecuteTool>::SharedPtr action_client;

  ToolEntry() : last_update(0, 0, RCL_ROS_TIME) {}
};

// ------------------------------------------------------------
// 管理节点
// ------------------------------------------------------------
class AgentOutputMgmt : public rclcpp::Node {
public:
  explicit AgentOutputMgmt(const std::string & agent_name)
  : Node("output_mgmt_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("tool_timeout", 60.0);
    this->declare_parameter<double>("info_timeout", 3.0);
    this->declare_parameter<double>("discovery_period", 1.0);

    tool_timeout_ = this->get_parameter("tool_timeout").as_double();
    info_timeout_ = this->get_parameter("info_timeout").as_double();
    double discovery_period = this->get_parameter("discovery_period").as_double();

    topic_prefix_ = "/" + agent_name_ + "/output/";

    get_tools_srv_ = this->create_service<GetToolsInfo>(
      topic_prefix_ + "info",
      std::bind(&AgentOutputMgmt::handle_get_tools, this, std::placeholders::_1, std::placeholders::_2));

    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      topic_prefix_.substr(0, topic_prefix_.size() - 1),
      [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
      // 取消回调：设置取消标志
      [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
        auto it = active_goals_.find(goal_handle->get_goal_id());
        if (it != active_goals_.end()) {
          it->second->canceled.store(true);
          RCLCPP_INFO(this->get_logger(), "管理节点收到取消请求");
        }
        return rclcpp_action::CancelResponse::ACCEPT;
      },
      // 接受回调
      [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
        auto state = std::make_shared<ExecutionState>();
        state->canceled.store(false);
        active_goals_[goal_handle->get_goal_id()] = state;
        std::thread{std::bind(&AgentOutputMgmt::execute, this, goal_handle, state)}.detach();
      }
    );

    discovery_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(discovery_period),
      std::bind(&AgentOutputMgmt::discover_tools, this));
    cleanup_timer_ = this->create_wall_timer(
      cloud_soul::CLEANUP_INTERVAL, std::bind(&AgentOutputMgmt::cleanup_tools, this));

    RCLCPP_INFO(this->get_logger(), "Agent output mgmt started for namespace: %s", agent_name_.c_str());
  }

private:
  struct ExecutionState {
    std::atomic<bool> canceled;
  };

  // ----------------------------------------------------------
  // 工具发现
  // ----------------------------------------------------------
  void discover_tools() {
    auto topics_and_types = this->get_topic_names_and_types();
    std::regex info_regex(topic_prefix_ + "([^/]+)/info");
    std::smatch match;

    for (const auto & [topic, types] : topics_and_types) {
      if (std::regex_match(topic, match, info_regex)) {
        std::string tool_name = match[1].str();
        if (tools_.find(tool_name) != tools_.end()) continue;

        RCLCPP_INFO(this->get_logger(), "发现新工具: %s", tool_name.c_str());
        auto entry = std::make_shared<ToolEntry>();
        entry->name = tool_name;
        entry->last_update = this->now();

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

  // ----------------------------------------------------------
  // 超时清理
  // ----------------------------------------------------------
  void cleanup_tools() {
    auto now = this->now();
    std::vector<std::string> to_remove;
    for (const auto & [name, entry] : tools_) {
      if ((now - entry->last_update).seconds() > info_timeout_) {
        to_remove.push_back(name);
      }
    }
    for (const auto & name : to_remove) {
      RCLCPP_WARN(this->get_logger(), "工具 %s 心跳超时，已移除", name.c_str());
      tools_.erase(name);
    }
  }

  // ----------------------------------------------------------
  // 服务回调
  // ----------------------------------------------------------
  void handle_get_tools(
    const std::shared_ptr<GetToolsInfo::Request> /*req*/,
    std::shared_ptr<GetToolsInfo::Response> res)
  {
    try {
      auto now = this->now();
      std::string tools_json = "[";
      bool first = true;
      for (const auto & [name, entry] : tools_) {
        if (entry->info_json.empty()) continue;
        if ((now - entry->last_update).seconds() > info_timeout_) continue;

        if (!first) tools_json += ",";
        first = false;
        tools_json += R"({"type":"function","function":)" + entry->info_json + "}";
      }
      tools_json += "]";
      res->tools_json = tools_json;
    } catch (const std::exception & e) {
      RCLCPP_ERROR(this->get_logger(), "构造工具列表异常: %s", e.what());
      res->tools_json = R"EOF({"error":"internal error while building tool list"})EOF";
    }
  }

  // ----------------------------------------------------------
  // 实际执行 (含动态超时、取消转发、工具 abort 透传)
  // ----------------------------------------------------------
  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle,
               std::shared_ptr<ExecutionState> state) {
    auto result = std::make_shared<ExecuteTool::Result>();
    const auto goal = goal_handle->get_goal();

    double effective_timeout = (goal->timeout_sec > 0.0) ? goal->timeout_sec : tool_timeout_;
    auto timeout_duration = std::chrono::duration<double>(effective_timeout);

    std::string compact_input = remove_json_whitespace(goal->input_json);

    std::string tool_name, arguments_json;
    try {
      tool_name = extract_json_string(compact_input, "name");
      arguments_json = extract_json_object(compact_input, "arguments");
      if (tool_name.empty()) throw std::runtime_error("missing 'name'");
    } catch (const std::exception & e) {
      result->output_json = R"EOF({"error":"invalid input json format"})EOF";
      result->exit_code = -1;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    auto it = tools_.find(tool_name);
    if (it == tools_.end()) {
      result->output_json = R"EOF({"error":"tool not found"})EOF";
      result->exit_code = -2;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    auto & entry = it->second;
    if (!entry->action_client) {
      result->output_json = R"EOF({"error":"tool action client not ready"})EOF";
      result->exit_code = -3;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    if (!entry->action_client->wait_for_action_server(1s)) {
      result->output_json = R"EOF({"error":"tool action server not available"})EOF";
      result->exit_code = -4;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    auto tool_goal = ExecuteTool::Goal();
    tool_goal.input_json = arguments_json;
    tool_goal.timeout_sec = effective_timeout;

    auto send_goal_future = entry->action_client->async_send_goal(tool_goal);
    if (send_goal_future.wait_for(1s) != std::future_status::ready) {
      result->output_json = R"EOF({"error":"tool did not accept goal"})EOF";
      result->exit_code = -5;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    auto tool_goal_handle = send_goal_future.get();
    if (!tool_goal_handle) {
      result->output_json = R"EOF({"error":"tool rejected goal"})EOF";
      result->exit_code = -6;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    // 等待工具结果，同时检测取消和超时
    auto result_future = entry->action_client->async_get_result(tool_goal_handle);
    auto start_time = std::chrono::steady_clock::now();
    bool canceled = false;

    while (true) {
      // 优先检查取消标志
      if (!canceled && state->canceled.load()) {
        canceled = true;
        RCLCPP_INFO(this->get_logger(), "管理节点取消工具执行: %s", tool_name.c_str());
        entry->action_client->async_cancel_goal(tool_goal_handle);
        // 取消后等待工具返回（最多给 1 秒）
        auto cancel_status = result_future.wait_for(1s);
        if (cancel_status == std::future_status::ready) {
          break;
        }
        // 即使工具未及时响应，也直接退出并返回取消信息
        result->output_json = R"EOF({"error":"execution canceled"})EOF";
        result->exit_code = -7;
        goal_handle->abort(result);
        active_goals_.erase(goal_handle->get_goal_id());
        return;
      }

      auto status = result_future.wait_for(std::chrono::milliseconds(100));
      if (status == std::future_status::ready) break;

      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (elapsed >= timeout_duration) {
        entry->action_client->async_cancel_goal(tool_goal_handle);
        result->output_json = R"EOF({"error":"tool execution timeout"})EOF";
        result->exit_code = -7;
        goal_handle->abort(result);
        active_goals_.erase(goal_handle->get_goal_id());
        return;
      }
    }

    auto wrapped_result = result_future.get();
    if (canceled) {
      result->output_json = R"EOF({"error":"execution canceled"})EOF";
      result->exit_code = -7;
      goal_handle->abort(result);
    } else if (wrapped_result.code == rclcpp_action::ResultCode::SUCCEEDED) {
      result->output_json = wrapped_result.result->output_json;
      result->exit_code = wrapped_result.result->exit_code;
      goal_handle->succeed(result);
    } else {
      // 工具 abort：透传原始错误
      if (wrapped_result.result) {
        result->output_json = wrapped_result.result->output_json;
        result->exit_code = wrapped_result.result->exit_code;
      } else {
        result->output_json = R"EOF({"error":"tool execution failed (action aborted)"})EOF";
        result->exit_code = -8;
      }
      goal_handle->abort(result);
    }

    active_goals_.erase(goal_handle->get_goal_id());
  }

  // ----------------------------------------------------------
  // 移除 JSON 结构空白
  // ----------------------------------------------------------
  static std::string remove_json_whitespace(const std::string & json) {
    std::string out;
    out.reserve(json.size());
    bool in_string = false;
    for (size_t i = 0; i < json.size(); ++i) {
      char c = json[i];
      if (in_string) {
        out.push_back(c);
        if (c == '"' && (i == 0 || json[i-1] != '\\')) {
          in_string = false;
        }
      } else {
        if (c == '"') {
          in_string = true;
          out.push_back(c);
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          continue;
        } else {
          out.push_back(c);
        }
      }
    }
    return out;
  }

  static std::string extract_json_string(const std::string & json, const std::string & key) {
    std::string search = "\"" + key + "\":\"";
    size_t start = json.find(search);
    if (start == std::string::npos) return "";
    start += search.length();
    size_t end = start;
    while (end < json.size()) {
      if (json[end] == '"' && (end == 0 || json[end-1] != '\\')) break;
      ++end;
    }
    return unescape_json(json.substr(start, end - start));
  }

  static std::string extract_json_object(const std::string & json, const std::string & key) {
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start == std::string::npos) return "{}";
    start += search.length();
    // 输入已无空白，直接期望 '{'
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

  static std::string unescape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        switch (s[i + 1]) {
          case '"':  out += '"';  ++i; break;
          case '\\': out += '\\'; ++i; break;
          case '/':  out += '/';  ++i; break;
          case 'n':  out += '\n'; ++i; break;
          case 'r':  out += '\r'; ++i; break;
          case 't':  out += '\t'; ++i; break;
          default:   out += '\\'; break;
        }
      } else {
        out += s[i];
      }
    }
    return out;
  }

  // 成员变量
  std::string agent_name_;
  std::string topic_prefix_;
  double tool_timeout_;
  double info_timeout_;

  rclcpp::Service<GetToolsInfo>::SharedPtr get_tools_srv_;
  rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;

  rclcpp::TimerBase::SharedPtr discovery_timer_;
  rclcpp::TimerBase::SharedPtr cleanup_timer_;

  std::unordered_map<std::string, std::shared_ptr<ToolEntry>> tools_;
  std::map<rclcpp_action::GoalUUID, std::shared_ptr<ExecutionState>> active_goals_;
};

// ----------------------------------------------------------
// main
// ----------------------------------------------------------
int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);

  auto temp_node = std::make_shared<rclcpp::Node>("temp");
  temp_node->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp_node->get_parameter("agent_name").as_string();
  temp_node.reset();

  auto node = std::make_shared<AgentOutputMgmt>(agent_name);

  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}