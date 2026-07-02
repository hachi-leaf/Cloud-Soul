// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul 标准 output 工具节点：消息发送
// unified message sending tool for Cloud-Soul

// Node: /<agent_name>/message_send_node
// Param:
//  <string>agent_name       --> Agent 名
//  <float64>info_rate       --> 发布 Tools Info 的频率（Hz）
//  <string>topic_output     --> ros_msg 渠道发布的话题名，默认 "raw_message"
//  <float64>default_timeout --> 默认状态下（Agent 将 Action 的 Goal 的 timeout_sec 设为 0 时）Action 的 timeout

// Topic: /<agent_name>/output/message_send/info
// Struct:
//  <string>info --> 输入给 LLM 的 tools json 字段，见 MESSAGE_SEND_INFO_JSON

// Topic: /<agent_name>/output/message_send/<topic_output>
// Struct:
//  <string>data --> ros_msg 渠道发布的消息内容，由 input_json 中的 "message" 字段决定

// Topic: /<agent_name>/output/message_send/web_chat
// Struct:
//  <string>data --> web_chat 渠道发布的消息内容，由 input_json 中的 "message" 字段决定

// Action: /<agent_name>/output/message_send
// Struct:
//  Goal <string>input_json      --> LLM 输出的 tools_call json 字段，由 MESSAGE_SEND_INFO_JSON 约束
//  Goal <float64>timeout_sec    --> Action 调用超时时间（秒），0 表示使用 default_timeout
//  ---
//  Results <string>output_json  --> 返回给 LLM tools_callback 字段，为自由字符串
//  Results <int32>exit_code     --> 错误码，0 为成功，-1 为错误
//  ---
//  Feedback <string>status      --> reserved

// Action 特性：
//  1. 禁止并行，并行时直接对新 Goal 返回 exit_code = -1, output_json = {"error":"Another goal is already running"}
//  2. 渠道 (channel) 必须为 email / ros_msg / web_chat，非法渠道返回 error: "unsupported channel"
//  3. email 渠道：
//     - 需系统安装 s-nail 并正确配置 ~/.mailrc，否则返回 error: "s-nail not available"
//     - 必须提供 to / subject / body 三个字段且非空，否则返回 error: "invalid email parameters"
//     - 发送成功返回 {"status":"sent"}，exit_code = 0
//     - 发送失败（s-nail 返回非零）返回 error 描述，exit_code = -1
//     - 注意：系统调用期间无法立即中止，但取消信号会在调用完成后处理
//  4. ros_msg 渠道：
//     - 必须提供非空 message，否则返回 error: "invalid message"
//     - 成功发布到话题后返回 {"status":"published"}，exit_code = 0
//  5. web_chat 渠道：
//     - 必须提供非空 message，否则返回 error: "invalid message"
//     - 成功发布到话题后返回 {"status":"published"}，exit_code = 0
//  6. 所有渠道支持超时：根据 timeout_sec 或 default_timeout 计算截止时间，超时未完成返回 error: "timed out after Xs"，exit_code = -1
//     - 注意：email 发送期间无法中断，超时检查仅在调用前后生效；调用过程中超时不会中止 s-nail 进程，但结果仍标记为超时
//  7. 用户主动 Cancel（包括 Ctrl+C 终止节点）：
//     - 对于 ros_msg / web_chat，立即返回 error: "execution canceled"
//     - 对于 email，已发送的邮件无法撤销，返回 error: "execution canceled"
//  8. 输入 JSON 格式错误时自动尝试修复（去尾逗号、补全括号等），修复仍失败则返回 error: "invalid input"
//  9. 所有输出 JSON 由 nlohmann::json 序列化，自动处理转义

// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul 标准 output 工具节点：消息发送
// unified message sending tool for Cloud-Soul

// Node: /<agent_name>/message_send_node
// Param:
//  <string>agent_name       --> Agent 名
//  <float64>info_rate       --> 发布 Tools Info 的频率（Hz）
//  <string>topic_output     --> ros_msg 渠道发布的话题名，默认 "raw_message"
//  <float64>default_timeout --> 默认状态下（Agent 将 Action 的 Goal 的 timeout_sec 设为 0 时）Action 的 timeout

// Topic: /<agent_name>/output/message_send/info
// Struct:
//  <string>info --> 输入给 LLM 的 tools json 字段，见 MESSAGE_SEND_INFO_JSON

// Action: /<agent_name>/output/message_send
// Struct:
//  Goal <string>input_json      --> LLM 输出的 tools_call json 字段，由 MESSAGE_SEND_INFO_JSON 约束
//  Goal <float64>timeout_sec    --> Action 调用超时时间（秒），0 表示使用 default_timeout
//  ---
//  Results <string>output_json  --> 返回给 LLM tools_callback 字段，为自由字符串
//  Results <int32>exit_code     --> 错误码，0 为成功，-1 为错误
//  ---
//  Feedback <string>status      --> reserved

// Action 特性：
//  1. 禁止并行，并行时直接对新 Goal 返回 exit_code = -1, output_json = {"error":"Another goal is already running"}
//  2. 渠道 (channel) 必须为 email / ros_msg / web_chat，非法渠道返回 error: "unsupported channel"
//  3. email 渠道：
//     - 需系统安装 s-nail 并正确配置 ~/.mailrc，否则返回 error: "s-nail not available"
//     - 必须提供 to / subject / body 三个字段且非空，否则返回 error: "invalid email parameters"
//     - 发送成功返回 {"status":"sent"}，exit_code = 0
//     - 发送失败（s-nail 返回非零）返回 error: "s-nail send failed"，exit_code = -1
//     - 注意：系统调用期间无法立即中止，但取消信号会在调用完成后处理
//  4. ros_msg 渠道：
//     - 必须提供非空 message，否则返回 error: "invalid message"
//     - 成功发布到话题后返回 {"status":"published"}，exit_code = 0
//  5. web_chat 渠道：
//     - 必须提供非空 message，否则返回 error: "invalid message"
//     - 成功发布到话题后返回 {"status":"published"}，exit_code = 0
//  6. 所有渠道支持超时：根据 timeout_sec 或 default_timeout 计算截止时间，超时未完成返回 error: "timed out after Xs"，exit_code = -1
//     - 注意：email 发送期间无法中断，超时检查仅在调用前后生效；调用过程中超时不会中止 s-nail 进程，但结果仍标记为超时
//  7. 用户主动 Cancel（包括 Ctrl+C 终止节点）：
//     - 对于 ros_msg / web_chat，立即返回 error: "execution canceled"
//     - 对于 email，已发送的邮件无法撤销，返回 error: "execution canceled"
//  8. 输入 JSON 格式错误时自动尝试修复（去尾逗号、补全括号等），修复仍失败则返回 error: "invalid input"
//  9. 所有输出 JSON 由 nlohmann::json 序列化，自动处理转义

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "nlohmann/json.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;
using json = nlohmann::json;

// ================================================================
// Tool Description (DeepSeek/OpenAI function-calling compatible)
// ================================================================
static constexpr const char* MESSAGE_SEND_INFO_JSON = R"json({
  "type": "function",
  "function": {
    "name": "message_send",
    "description": "发送消息到不同渠道。支持 email (需系统安装 s-nail 并配置 ~/.mailrc)、ros_msg (ROS2 话题发布)、web_chat (网页聊天输出)。\n\n场景示例:\n  - 发送邮件: {\"channel\":\"email\",\"to\":\"someone@example.com\",\"subject\":\"你好\",\"body\":\"邮件内容\"}\n  - 发布到 ROS 话题: {\"channel\":\"ros_msg\",\"message\":\"hello world\"}\n  - 发送到网页聊天: {\"channel\":\"web_chat\",\"message\":\"hello web\"}\n\n规则:\n  - channel 必须为 email / ros_msg / web_chat 之一\n  - email 渠道需要 to, subject, body 三个字段，不能为空\n  - ros_msg 和 web_chat 渠道需要 message 字段，不能为空\n  - email 发送依赖 s-nail，发送期间可能阻塞，请设置合理超时\n\n返回值:\n  - 成功: {\"status\":\"sent\"} 或 {\"status\":\"published\"}\n  - 任何错误: {\"error\":\"错误原因\"}",
    "parameters": {
      "type": "object",
      "required": ["channel"],
      "properties": {
        "channel": {
          "type": "string",
          "enum": ["email", "ros_msg", "web_chat"],
          "description": "消息渠道"
        },
        "to": {
          "type": "string",
          "description": "收件人 (email 渠道)"
        },
        "subject": {
          "type": "string",
          "description": "主题 (email 渠道)"
        },
        "body": {
          "type": "string",
          "description": "邮件正文 (email 渠道)"
        },
        "message": {
          "type": "string",
          "description": "ros_msg, web_chat 渠道消息内容 (ros_msg 是命令行，不支持 MarkDown 渲染，web_chat 支持完整 MarkDown 渲染，建议使用 MarkDown 语法)"
        }
      }
    }
  }
})json";

// JSON 修复函数 (与 shell_exec 一致)
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

class MessageSendNode : public rclcpp::Node {
public:
  explicit MessageSendNode(const std::string & agent_name)
  : Node("message_send_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);
    this->declare_parameter<std::string>("topic_output", "raw_message");
    this->declare_parameter<double>("default_timeout", 30.0);

    double info_rate = this->get_parameter("info_rate").as_double();
    topic_output_ = this->get_parameter("topic_output").as_string();
    default_timeout_ = this->get_parameter("default_timeout").as_double();

    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output/message_send",
      [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
      [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
        std::lock_guard<std::mutex> lock(active_mutex_);
        if (auto it = active_goals_.find(goal_handle->get_goal_id());
            it != active_goals_.end()) {
          it->second->canceled.store(true);
          RCLCPP_INFO(this->get_logger(), "收到取消请求");
        }
        return rclcpp_action::CancelResponse::ACCEPT;
      },
      [this](auto goal_handle) {
        {
          std::lock_guard<std::mutex> lock(active_mutex_);
          if (!active_goals_.empty()) {
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"Another goal is already running"})";
            result->exit_code = -1;
            goal_handle->abort(result);
            RCLCPP_WARN(this->get_logger(), "拒绝新目标：已有消息正在发送");
            return;
          }
          auto exec_state = std::make_shared<ExecutionState>();
          exec_state->canceled.store(false);
          active_goals_[goal_handle->get_goal_id()] = exec_state;
        }
        std::thread{std::bind(&MessageSendNode::execute, this, goal_handle,
                              active_goals_[goal_handle->get_goal_id()])}.detach();
      }
    );

    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/message_send/info", qos);
    publish_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / info_rate),
      [this]() { publish_info(); });
    publish_info();

    topic_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/message_send/" + topic_output_, 10);
    web_chat_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/message_send/web_chat", 10);
  }

private:
  struct ExecutionState {
    std::atomic<bool> canceled;
  };

  void publish_info() {
    std_msgs::msg::String msg;
    msg.data = MESSAGE_SEND_INFO_JSON;
    info_pub_->publish(msg);
  }

  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle,
               std::shared_ptr<ExecutionState> exec_state) {
    auto result = std::make_shared<ExecuteTool::Result>();
    try {
      const auto goal = goal_handle->get_goal();

      // 解析 JSON，带修复
      json raw;
      try {
        raw = json::parse(goal->input_json);
      } catch (const json::parse_error &) {
        std::string fixed = repair_json(goal->input_json);
        try {
          raw = json::parse(fixed);
          RCLCPP_INFO(this->get_logger(), "JSON 自动修复成功");
        } catch (const std::exception &) {
          result->output_json = R"({"error":"invalid input"})";
          result->exit_code = -1;
          goal_handle->abort(result);
          {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_goals_.erase(goal_handle->get_goal_id());
          }
          return;
        }
      }

      json args = raw.contains("arguments") && raw["arguments"].is_object() ? raw["arguments"] : json::object();
      if (!args.contains("channel") || !args["channel"].is_string()) {
        result->output_json = R"({"error":"invalid input: channel is required"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        {
          std::lock_guard<std::mutex> lock(active_mutex_);
          active_goals_.erase(goal_handle->get_goal_id());
        }
        return;
      }
      std::string channel = args["channel"].get<std::string>();

      // 计算超时截止时间（使用整数秒 duration）
      double timeout = default_timeout_;
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>(timeout));

      if (channel == "email") {
        handle_email(args, result, goal_handle, exec_state, deadline);
      } else if (channel == "ros_msg") {
        handle_ros_msg(args, result, goal_handle, exec_state, deadline);
      } else if (channel == "web_chat") {
        handle_web_chat(args, result, goal_handle, exec_state, deadline);
      } else {
        result->output_json = R"({"error":"unsupported channel"})";
        result->exit_code = -1;
        goal_handle->abort(result);
      }
    } catch (const std::exception & e) {
      result->output_json = R"({"error":"internal error: )" + std::string(e.what()) + R"("})";
      result->exit_code = -1;
      goal_handle->abort(result);
    } catch (...) {
      result->output_json = R"({"error":"internal unknown error"})";
      result->exit_code = -1;
      goal_handle->abort(result);
    }
    {
      std::lock_guard<std::mutex> lock(active_mutex_);
      active_goals_.erase(goal_handle->get_goal_id());
    }
  }

  void handle_email(json &args,
                    std::shared_ptr<ExecuteTool::Result> result,
                    const std::shared_ptr<GoalHandleExecute> goal_handle,
                    std::shared_ptr<ExecutionState> exec_state,
                    std::chrono::steady_clock::time_point deadline) {
    const auto goal = goal_handle->get_goal();
    double timeout = default_timeout_;

    if (system("command -v s-nail > /dev/null 2>&1") != 0) {
      result->output_json = R"({"error":"s-nail not available"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    if (!args.contains("to") || !args["to"].is_string() ||
        !args.contains("subject") || !args["subject"].is_string() ||
        !args.contains("body") || !args["body"].is_string()) {
      result->output_json = R"({"error":"invalid email parameters"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }
    std::string to = args["to"].get<std::string>();
    std::string subject = args["subject"].get<std::string>();
    std::string body = args["body"].get<std::string>();
    if (to.empty() || subject.empty()) {
      result->output_json = R"({"error":"invalid email parameters"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    // 调用前超时检查
    if (std::chrono::steady_clock::now() > deadline) {
      result->output_json = R"({"error":"timed out after )" + std::to_string(timeout) + "s\"}";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    auto escape = [](const std::string &s) {
      std::string out;
      for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
      }
      return out;
    };
    std::string cmd = "echo '" + escape(body) + "' | s-nail -s '" +
                      escape(subject) + "' " + to;
    RCLCPP_INFO(this->get_logger(), "发送邮件至 %s", to.c_str());

    int ret = system(cmd.c_str());

    // 调用后检查取消
    if (exec_state->canceled.load()) {
      result->output_json = R"({"error":"execution canceled"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    // 调用后超时检查
    if (std::chrono::steady_clock::now() > deadline) {
      result->output_json = R"({"error":"timed out after )" + std::to_string(timeout) + "s\"}";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    if (ret != 0) {
      result->output_json = R"({"error":"s-nail send failed"})";
      result->exit_code = -1;
      goal_handle->abort(result);
    } else {
      result->output_json = R"({"status":"sent"})";
      result->exit_code = 0;
      goal_handle->succeed(result);
    }
  }

  void handle_ros_msg(json &args,
                      std::shared_ptr<ExecuteTool::Result> result,
                      const std::shared_ptr<GoalHandleExecute> goal_handle,
                      std::shared_ptr<ExecutionState> exec_state,
                      std::chrono::steady_clock::time_point deadline) {
    const auto goal = goal_handle->get_goal();
    double timeout = default_timeout_;

    if (std::chrono::steady_clock::now() > deadline) {
      result->output_json = R"({"error":"timed out after )" + std::to_string(timeout) + "s\"}";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }
    if (exec_state->canceled.load()) {
      result->output_json = R"({"error":"execution canceled"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    if (!args.contains("message") || !args["message"].is_string()) {
      result->output_json = R"({"error":"invalid message"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }
    std::string msg_str = args["message"].get<std::string>();
    if (msg_str.empty()) {
      result->output_json = R"({"error":"invalid message"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    auto msg = std::make_unique<std_msgs::msg::String>();
    msg->data = msg_str;
    topic_pub_->publish(std::move(msg));

    result->output_json = R"({"status":"published"})";
    result->exit_code = 0;
    goal_handle->succeed(result);
  }

  void handle_web_chat(json &args,
                       std::shared_ptr<ExecuteTool::Result> result,
                       const std::shared_ptr<GoalHandleExecute> goal_handle,
                       std::shared_ptr<ExecutionState> exec_state,
                       std::chrono::steady_clock::time_point deadline) {
    const auto goal = goal_handle->get_goal();
    double timeout = default_timeout_;

    if (std::chrono::steady_clock::now() > deadline) {
      result->output_json = R"({"error":"timed out after )" + std::to_string(timeout) + "s\"}";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }
    if (exec_state->canceled.load()) {
      result->output_json = R"({"error":"execution canceled"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    if (!args.contains("message") || !args["message"].is_string()) {
      result->output_json = R"({"error":"invalid message"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }
    std::string msg_str = args["message"].get<std::string>();
    if (msg_str.empty()) {
      result->output_json = R"({"error":"invalid message"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    auto msg = std::make_unique<std_msgs::msg::String>();
    msg->data = msg_str;
    web_chat_pub_->publish(std::move(msg));

    result->output_json = R"({"status":"published"})";
    result->exit_code = 0;
    goal_handle->succeed(result);
  }

  std::string agent_name_;
  std::string topic_output_;
  double default_timeout_ = 30.0;

  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr topic_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr web_chat_pub_;
  rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
  rclcpp::TimerBase::SharedPtr publish_timer_;

  std::mutex active_mutex_;
  std::map<rclcpp_action::GoalUUID, std::shared_ptr<ExecutionState>> active_goals_;
};

int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);
  auto temp = std::make_shared<rclcpp::Node>("temp");
  temp->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp->get_parameter("agent_name").as_string();
  temp.reset();
  auto node = std::make_shared<MessageSendNode>(agent_name);
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
