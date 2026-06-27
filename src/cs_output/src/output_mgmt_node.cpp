// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul 核心管理节点：统一输出管理器
// unified output management node for Cloud-Soul

// Node: /<agent_name>/output_mgmt_node
// Param:
//  <string>agent_name        --> Agent 名
//  <float64>info_timeout     --> 工具心跳超时时间（秒），默认 3.0
//  <float64>discovery_period --> 工具发现检查周期（秒），默认 1.0
//  <float64>default_timeout  --> 默认 Action 超时（秒），当 Agent 的 timeout_sec 设为 0 时使用，默认 60.0
//  <float64>cancel_timeout   --> 取消等待超时（秒），管理节点 Cancel 子工具后等待其返回的最长时间，默认 2.0

// Service: /<agent_name>/output/info
// Struct:
//  Request  <>             --> 空
//  ---
//  Response <string>tools_json --> 当前在线的工具描述 JSON 数组

// Action: /<agent_name>/output
// Struct:
//  Goal <string>input_json      --> LLM 输出的 tools_call json 字段，必须包含 "name" 和 "arguments"
//  Goal <float64>timeout_sec    --> Action 调用超时时间（秒），0 表示使用 default_timeout
//  ---
//  Results <string>output_json  --> 返回给 LLM tools_callback 字段，为自由字符串
//  Results <int32>exit_code     --> 错误码，0 为成功，-1 为错误
//  ---
//  Feedback <string>status      --> reserved

// Action 特性：
//  1. 工具发现：自动发现 /<agent_name>/output/*/info 话题上的工具节点，通过心跳维持在线状态
//  2. 心跳超时：工具 info 超过 info_timeout 未更新则标记离线，从工具列表中移除；恢复发布后重新上线
//  3. 统一代理：接收 LLM tool call，解析 "name" 字段，路由到对应工具节点的 Action，透传 "arguments" 和 timeout_sec
//  4. 输入校验：
//     - input_json 必须为合法 JSON 且包含 "name" 字段，否则返回 exit_code = -1, output_json = {"error":"invalid input json"}
//     - input_json 格式错误时自动尝试修复（去尾逗号、补全括号），修复失败返回上述错误
//  5. 工具不存在：如果 "name" 对应的工具不在线（从未上线、心跳超时移除），返回 exit_code = -1, output_json = {"error":"tool not found"}
//  6. 并发控制：同一工具同时只允许一个 Goal 执行，若已有 Goal 在进行中则拒绝新 Goal，返回 exit_code = -1, output_json = {"error":"tool is busy"}
//  7. 超时控制：
//     - 使用 timeout_sec 或 default_timeout 作为截止时间
//     - 管理节点等待工具响应超时 → exit_code = -1, output_json = {"error":"tool execution timeout"}
//     - 超时后管理节点主动 Cancel 子工具 Goal，并等待 cancel_timeout 秒
//  8. 取消转发：
//     - 上层 Cancel Goal 时，管理节点转发取消给子工具
//     - 若子工具在 cancel_timeout 内返回取消结果，管理节点透传该结果（exit_code 规则同下）
//     - 若超时后子工具仍未返回，管理节点放弃等待，返回 exit_code = -1, output_json = {"error":"子工具 Action 无法返回"}
//  9. 结果透传：子工具返回的 exit_code 和 output_json 原样透传
//     - 子工具 exit_code == 0 → 管理节点 exit_code = 0
//     - 子工具 exit_code != 0 → 管理节点 exit_code = -1
// 10. 工具连接丢失：若子工具 Action Server 不可用（崩溃、断连），返回 exit_code = -1, output_json = {"error":"tool connection lost"}
// 11. 防御性捕获：调用子工具过程中发生未捕获的 C++ 异常时，返回 exit_code = -1, output_json = {"error":"子工具发生了未捕捉的错误"}
// 12. 节点关闭：收到 SIGINT 后取消 discovery_timer_，设置 shutting_down_ 标志让等待线程快速退出，spin 返回，进程正常结束
// 13. 所有管理节点自身产生的错误均使用自定义的 output_json 错误文本，子工具返回的结果原样透传
// 14. 工具 info 无效 JSON 自动跳过，不影响其他工具及服务响应

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

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

// JSON 修复函数（尾逗号、括号不全等常见 LLM 输出错误）
static std::string repair_json(const std::string& raw) {
    std::string s = raw;
    size_t p0 = s.find_first_not_of(" \t\n\r");
    if (p0 == std::string::npos) return s;
    size_t p1 = s.find_last_not_of(" \t\n\r");
    s = s.substr(p0, p1 - p0 + 1);
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ',' && i + 1 < s.size())
            if (s[i+1] == '}' || s[i+1] == ']') continue;
        r += s[i];
    }
    s = r;
    int brace = 0, brack = 0;
    bool instr = false, esc = false;
    for (char c : s) {
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { instr = !instr; continue; }
        if (instr) continue;
        if (c == '{') brace++;
        if (c == '}') brace--;
        if (c == '[') brack++;
        if (c == ']') brack--;
    }
    const char closer[] = {'}',']',0};
    size_t lc = s.find_last_of(closer);
    if (lc != std::string::npos) {
        s = s.substr(0, lc + 1);
        brace = 0; brack = 0; instr = false; esc = false;
        for (char c : s) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') { instr = !instr; continue; }
            if (instr) continue;
            if (c == '{') brace++;
            if (c == '}') brace--;
            if (c == '[') brack++;
            if (c == ']') brack--;
        }
    }
    while (brack > 0) { s += ']'; brack--; }
    while (brace > 0) { s += '}'; brace--; }
    return s;
}

class OutputMgmtNode : public rclcpp::Node {
public:
  OutputMgmtNode(const std::string & agent_name)
  : Node("output_mgmt_node", agent_name), agent_name_(agent_name)
  {
    declare_parameter("agent_name", agent_name);
    declare_parameter("info_timeout", 3.0);
    declare_parameter("discovery_period", 1.0);
    declare_parameter("default_timeout", 60.0);
    declare_parameter("cancel_timeout", 2.0);

    info_timeout_ = get_parameter("info_timeout").as_double();
    discovery_period_ = get_parameter("discovery_period").as_double();
    default_timeout_ = get_parameter("default_timeout").as_double();
    cancel_timeout_ = get_parameter("cancel_timeout").as_double();

    // 节点关闭回调：取消定时器，设置关闭标志，使 spin 能退出，Goal 线程能快速结束
    rclcpp::on_shutdown([this]() {
      shutting_down_.store(true);
      if (discovery_timer_) {
        discovery_timer_->cancel();
      }
      RCLCPP_INFO(get_logger(), "节点正在关闭");
    });

    // 工具列表查询服务
    info_srv_ = create_service<GetToolsInfo>(
      "/" + agent_name_ + "/output/info",
      [this](const std::shared_ptr<GetToolsInfo::Request>,
             std::shared_ptr<GetToolsInfo::Response> response) {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        json tools_arr = json::array();
        for (const auto & [name, info] : tools_) {
          try {
            tools_arr.push_back(json::parse(info->description_json));
          } catch (...) {} // 跳过无效 JSON
        }
        response->tools_json = tools_arr.dump();
      });

    // 统一动作服务器
    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output",
      [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
      [this](auto goal_handle) {
        RCLCPP_INFO(get_logger(), "收到上层取消请求");
        return rclcpp_action::CancelResponse::ACCEPT;
      },
      [this](auto goal_handle) {
        std::thread{std::bind(&OutputMgmtNode::handle_goal, this, goal_handle)}.detach();
      });

    // 工具发现定时器
    discovery_timer_ = create_wall_timer(
      std::chrono::duration<double>(discovery_period_),
      [this]() { discover_tools(); });

    RCLCPP_INFO(get_logger(), "output_mgmt_node started");
  }

private:
  struct ToolInfo {
    std::string name;
    std::string description_json;
    rclcpp::Time last_seen;
    rclcpp_action::Client<ExecuteTool>::SharedPtr action_client;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr info_sub;
    std::atomic<bool> busy{false}; // 并发控制标志
  };

  std::atomic<bool> shutting_down_{false}; // 节点关闭标志

  // 工具发现与心跳管理
  void discover_tools() {
    auto topic_names = get_topic_names_and_types();
    std::string prefix = "/" + agent_name_ + "/output/";
    std::string info_suffix = "/info";

    for (const auto & [topic_name, types] : topic_names) {
      if (topic_name.find(prefix) != 0) continue;
      if (topic_name.find(info_suffix) == std::string::npos) continue;
      if (topic_name.size() <= prefix.size() + info_suffix.size()) continue;

      size_t tool_start = prefix.size();
      size_t tool_end = topic_name.find(info_suffix);
      if (tool_end <= tool_start) continue;
      std::string tool_name = topic_name.substr(tool_start, tool_end - tool_start);

      {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        if (tools_.find(tool_name) != tools_.end()) continue;
      }

      auto tool = std::make_shared<ToolInfo>();
      tool->name = tool_name;
      tool->last_seen = now();

      tool->info_sub = create_subscription<std_msgs::msg::String>(
        topic_name, rclcpp::QoS(1).reliable().transient_local(),
        [this, tool_name](const std_msgs::msg::String::SharedPtr msg) {
          std::lock_guard<std::mutex> lock(tools_mutex_);
          auto it = tools_.find(tool_name);
          if (it != tools_.end()) {
            it->second->description_json = msg->data;
            it->second->last_seen = now();
          }
        });

      std::string action_name = "/" + agent_name_ + "/output/" + tool_name;
      tool->action_client = rclcpp_action::create_client<ExecuteTool>(this, action_name);

      {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        tools_[tool_name] = tool;
      }
      RCLCPP_INFO(get_logger(), "发现工具: %s", tool_name.c_str());
    }

    // 心跳超时移除离线工具
    auto now_time = now();
    std::lock_guard<std::mutex> lock(tools_mutex_);
    std::vector<std::string> to_remove;
    for (const auto & [name, info] : tools_) {
      if ((now_time - info->last_seen).seconds() > info_timeout_) {
        to_remove.push_back(name);
      }
    }
    for (const auto & name : to_remove) {
      tools_.erase(name);
      RCLCPP_WARN(get_logger(), "工具心跳超时移除: %s", name.c_str());
    }
  }

  // 核心 Goal 处理逻辑
  void handle_goal(const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle) {
    auto result = std::make_shared<ExecuteTool::Result>();
    std::shared_ptr<ToolInfo> tool;
    try {
      const auto goal = goal_handle->get_goal();
      double timeout = goal->timeout_sec > 0.0 ? goal->timeout_sec : default_timeout_;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
      auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(cancel_timeout_);

      // 解析输入 JSON，带修复
      json input;
      try {
        input = json::parse(goal->input_json);
      } catch (const json::parse_error &) {
        std::string fixed = repair_json(goal->input_json);
        try {
          input = json::parse(fixed);
          RCLCPP_INFO(get_logger(), "JSON 自动修复成功");
        } catch (const std::exception &) {
          result->output_json = R"({"error":"invalid input json"})";
          result->exit_code = -1;
          goal_handle->abort(result);
          return;
        }
      }

      if (!input.contains("name") || !input["name"].is_string()) {
        result->output_json = R"({"error":"invalid input json"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        return;
      }
      std::string tool_name = input["name"].get<std::string>();

      // 查找工具并检查并发
      {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        auto it = tools_.find(tool_name);
        if (it == tools_.end()) {
          result->output_json = R"({"error":"tool not found"})";
          result->exit_code = -1;
          goal_handle->abort(result);
          return;
        }
        tool = it->second;
        if (tool->busy.exchange(true)) {
          result->output_json = R"({"error":"tool is busy"})";
          result->exit_code = -1;
          goal_handle->abort(result);
          return;
        }
      }
      auto release = [&tool]() { if (tool) tool->busy.store(false); };

      // 节点正在关闭则立即退出
      if (shutting_down_.load()) {
        result->output_json = R"({"error":"node shutting down"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        release();
        return;
      }

      // 构造子工具请求
      auto tool_goal = ExecuteTool::Goal();
      if (input.contains("arguments")) {
        try {
          tool_goal.input_json = input["arguments"].dump();
        } catch (const std::exception &) {
          result->output_json = R"({"error":"子工具发生了未捕捉的错误"})";
          result->exit_code = -1;
          goal_handle->abort(result);
          release();
          return;
        }
      }
      tool_goal.timeout_sec = goal->timeout_sec;

      // 检查子工具 Action Server 是否可用
      if (!tool->action_client->wait_for_action_server(std::chrono::seconds(1))) {
        result->output_json = R"({"error":"tool connection lost"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        release();
        return;
      }

      auto send_future = tool->action_client->async_send_goal(tool_goal);

      // 等待子工具响应、超时或取消
      while (std::chrono::steady_clock::now() < deadline) {
        if (shutting_down_.load()) {
          result->output_json = R"({"error":"node shutting down"})";
          result->exit_code = -1;
          goal_handle->abort(result);
          release();
          return;
        }
        if (send_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
          auto tool_goal_handle = send_future.get();
          if (!tool_goal_handle) {
            result->output_json = R"({"error":"tool connection lost"})";
            result->exit_code = -1;
            goal_handle->abort(result);
            release();
            return;
          }

          auto result_future = tool->action_client->async_get_result(tool_goal_handle);
          while (std::chrono::steady_clock::now() < deadline) {
            if (shutting_down_.load()) {
              result->output_json = R"({"error":"node shutting down"})";
              result->exit_code = -1;
              goal_handle->abort(result);
              release();
              return;
            }
            if (result_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
              auto wrapped = result_future.get();
              result->output_json = wrapped.result->output_json;
              result->exit_code = (wrapped.result->exit_code == 0) ? 0 : -1;
              goal_handle->succeed(result);
              release();
              return;
            }
          }

          // 超时，尝试取消子工具
          tool->action_client->async_cancel_goal(tool_goal_handle);
          while (std::chrono::steady_clock::now() < cancel_deadline) {
            if (shutting_down_.load()) {
              result->output_json = R"({"error":"node shutting down"})";
              result->exit_code = -1;
              goal_handle->abort(result);
              release();
              return;
            }
            if (result_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
              auto wrapped = result_future.get();
              result->output_json = wrapped.result->output_json;
              result->exit_code = (wrapped.result->exit_code == 0) ? 0 : -1;
              goal_handle->succeed(result);
              release();
              return;
            }
          }

          // 取消放弃：子工具未在 cancel_timeout 内返回
          result->output_json = R"({"error":"子工具 Action 无法返回"})";
          result->exit_code = -1;
          goal_handle->abort(result);
          release();
          return;
        }
      }

      // 发送超时（极难触发）
      result->output_json = R"({"error":"tool execution timeout"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      release();
    } catch (const std::exception &) {
      if (tool) tool->busy.store(false);
      result->output_json = R"({"error":"子工具发生了未捕捉的错误"})";
      result->exit_code = -1;
      goal_handle->abort(result);
    } catch (...) {
      if (tool) tool->busy.store(false);
      result->output_json = R"({"error":"子工具发生了未捕捉的错误"})";
      result->exit_code = -1;
      goal_handle->abort(result);
    }
  }

  std::string agent_name_;
  double info_timeout_;
  double discovery_period_;
  double default_timeout_;
  double cancel_timeout_;

  rclcpp::Service<GetToolsInfo>::SharedPtr info_srv_;
  rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
  rclcpp::TimerBase::SharedPtr discovery_timer_;

  std::mutex tools_mutex_;
  std::map<std::string, std::shared_ptr<ToolInfo>> tools_;
};

int main(int argc, char ** argv) {
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