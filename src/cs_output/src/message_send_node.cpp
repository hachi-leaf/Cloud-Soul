// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: /<agent_name>/message_send_node (工具节点，由 output_mgmt_node 自动发现)
// 作用: 消息发送器，支持多种渠道（邮件 / ROS 2 话题 / web_chat），可扩展飞书、微信等。
//       邮件使用系统 s-nail 发送（依赖 ~/.mailrc 配置）。
//       超时与取消由上层 output_mgmt_node 统一管理。
//
// =============================================================================
// 【邮件配置与验证方法】
// 1. 安装 s-nail
//    sudo apt update && sudo apt install s-nail
//
// 2. 配置发件邮箱 (~/.mailrc)
//    以 163 邮箱为例，请根据实际邮箱修改：
//
//    set v15-compat
//    set mta=smtp://你的邮箱%40域名:授权码@smtp.163.com
//    set from=你的邮箱@163.com
//
//    注意：
//    - 邮箱中的 @ 必须替换为 %40
//    - 密码部分需使用邮箱生成的【授权码】，不是登录密码
//    - 其他邮箱（QQ、Gmail）请替换对应的 SMTP 服务器地址和端口
//
// 3. 验证配置
//    echo "测试邮件" | s-nail -s "测试主题" 收件人@163.com
//    若能收到邮件，则配置成功，本节点即可正常发送邮件。
//
// 若未完成以上配置，当调用 email 渠道时会返回错误（exit_code = -3），不影响其他功能。
// =============================================================================
//
// 参数:
//   agent_name   - 命名空间，默认 "agent"
//   info_rate    - info 话题发布频率(Hz)，默认 1.0
//   topic_output - 话题输出渠道的 ROS 2 话题名，默认 "raw_message"
//
// 动作: /<agent_name>/output/message_send (ExecuteTool)
//   goal: input_json 示例:
//     {"channel":"email","to":"someone@example.com","subject":"你好","body":"邮件内容"}
//     {"channel":"ros_msg","message":"hello world"}
//     {"channel":"web_chat","message":"hello web"}
//   result: output_json 包含 status 或 error
//
// 话题: /<agent_name>/output/message_send/info (工具描述)
//       /<agent_name>/output/message_send/<topic_output> (渠道为 ros_msg 时的输出)
//       /<agent_name>/output/message_send/web_chat (渠道为 web_chat 时的输出)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "cs_interfaces/constants.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

class MessageSendNode : public rclcpp::Node {
public:
  explicit MessageSendNode(const std::string & agent_name)
  : Node("message_send_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);
    this->declare_parameter<std::string>("topic_output", "raw_message");

    double info_rate = this->get_parameter("info_rate").as_double();
    topic_output_ = this->get_parameter("topic_output").as_string();

    // 动作服务器
    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output/message_send",
      [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
      [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
        // 取消请求：s-nail 发送期间无法中断，但仍接受请求
        auto it = active_goals_.find(goal_handle->get_goal_id());
        if (it != active_goals_.end()) {
          it->second->canceled.store(true);
          RCLCPP_INFO(this->get_logger(), "收到取消请求（邮件发送无法立即中止）");
        }
        return rclcpp_action::CancelResponse::ACCEPT;
      },
      [this](auto goal_handle) {
        auto state = std::make_shared<ExecutionState>();
        state->canceled.store(false);
        active_goals_[goal_handle->get_goal_id()] = state;
        std::thread{std::bind(&MessageSendNode::execute, this, goal_handle, state)}.detach();
      }
    );

    // info 话题
    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/message_send/info", qos);
    publish_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / info_rate),
      [this]() { publish_info(); });
    publish_info();

    // 话题输出发布者 (ros_msg 渠道)
    topic_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/message_send/" + topic_output_, 10);

    // web_chat 输出发布者
    web_chat_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/message_send/web_chat", 10);

    RCLCPP_INFO(this->get_logger(), "MessageSend node ready");
  }

private:
  struct ExecutionState {
    std::atomic<bool> canceled;
  };

  void publish_info() {
    std_msgs::msg::String msg;
    msg.data = R"json(
      {
        "name": "message_send",
        "description": "发送消息，支持 channel=email (需系统安装 s-nail 并配置 ~/.mailrc) 或 channel=ros_msg (ROS2 String 发布) 或 channel=web_chat (web 聊天)。",
        "parameters": {
          "type": "object",
          "properties": {
            "channel": {
              "type": "string",
              "enum": ["email","ros_msg","web_chat"],
              "description": "消息渠道"
            },
            "to":         { "type": "string", "description": "收件人 (email 渠道)" },
            "subject":    { "type": "string", "description": "主题 (email 渠道)" },
            "body":       { "type": "string", "description": "邮件正文/消息内容" },
            "message":    { "type": "string", "description": "消息内容 (ros_msg / web_chat 渠道)" }
          },
          "required": ["channel"]
        }
      }
    )json";
    info_pub_->publish(msg);
  }

  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle,
               std::shared_ptr<ExecutionState> exec_state) {
    auto result = std::make_shared<ExecuteTool::Result>();
    const auto goal = goal_handle->get_goal();
    const std::string & input_json = goal->input_json;

    std::string channel;
    try {
      channel = extract_json_string(input_json, "channel");
      if (channel.empty()) throw std::runtime_error("missing channel");
    } catch (...) {
      result->output_json = cloud_soul::Msg::JSON_INVALID_INPUT;
      result->exit_code = cloud_soul::Err::MsgSend::INVALID_INPUT;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    if (channel == "email") {
      handle_email(input_json, result, goal_handle);
    } else if (channel == "ros_msg") {
      handle_topic(input_json, result, goal_handle);
    } else if (channel == "web_chat") {
      handle_web_chat(input_json, result, goal_handle);
    } else {
      result->output_json = cloud_soul::Msg::JSON_UNSUPPORTED_CHAN;
      result->exit_code = cloud_soul::Err::MsgSend::UNSUPPORTED_CHAN;
      goal_handle->abort(result);
    }

    active_goals_.erase(goal_handle->get_goal_id());
  }

  void handle_email(const std::string & input_json,
                    std::shared_ptr<ExecuteTool::Result> result,
                    const std::shared_ptr<GoalHandleExecute> goal_handle) {
    // 检查 s-nail 是否可用
    if (system("command -v s-nail > /dev/null 2>&1") != 0) {
      result->output_json = cloud_soul::Msg::JSON_SNAIL_NOT_INST;
      result->exit_code = cloud_soul::Err::MsgSend::SNAIL_UNAVAIL;
      goal_handle->abort(result);
      return;
    }

    std::string to, subject, body;
    try {
      to = extract_json_string(input_json, "to");
      subject = extract_json_string(input_json, "subject");
      body = extract_json_string(input_json, "body");
      if (to.empty() || subject.empty()) throw std::runtime_error("missing fields");
    } catch (...) {
      result->output_json = cloud_soul::Msg::JSON_INVALID_EMAIL;
      result->exit_code = cloud_soul::Err::MsgSend::INVALID_INPUT;
      goal_handle->abort(result);
      return;
    }

    // 使用 s-nail 发送邮件（依赖 ~/.mailrc 配置）
    std::string cmd = "echo '" + escape_shell(body) + "' | s-nail -s '" +
                      escape_shell(subject) + "' " + to;
    RCLCPP_INFO(this->get_logger(), "发送邮件至 %s", to.c_str());

    int ret = system(cmd.c_str());
    if (ret != 0) {
      result->output_json = cloud_soul::Msg::JSON_SNAIL_SEND_FAIL;
      result->exit_code = cloud_soul::Err::MsgSend::SNAIL_UNAVAIL;
      goal_handle->abort(result);
    } else {
      result->output_json = cloud_soul::Msg::JSON_STATUS_SENT;
      result->exit_code = 0;
      goal_handle->succeed(result);
    }
  }

  void handle_topic(const std::string & input_json,
                    std::shared_ptr<ExecuteTool::Result> result,
                    const std::shared_ptr<GoalHandleExecute> goal_handle) {
    std::string message;
    try {
      message = extract_json_string(input_json, "message");
      if (message.empty()) throw std::runtime_error("missing message");
    } catch (...) {
      result->output_json = cloud_soul::Msg::JSON_INVALID_TOPIC;
      result->exit_code = cloud_soul::Err::MsgSend::INVALID_INPUT;
      goal_handle->abort(result);
      return;
    }

    auto msg = std::make_unique<std_msgs::msg::String>();
    msg->data = message;
    topic_pub_->publish(std::move(msg));

    result->output_json = cloud_soul::Msg::JSON_STATUS_PUBLISHED;
    result->exit_code = 0;
    goal_handle->succeed(result);
  }

  void handle_web_chat(const std::string & input_json,
                       std::shared_ptr<ExecuteTool::Result> result,
                       const std::shared_ptr<GoalHandleExecute> goal_handle) {
    std::string message;
    try {
      message = extract_json_string(input_json, "message");
      if (message.empty()) throw std::runtime_error("missing message");
    } catch (...) {
      result->output_json = cloud_soul::Msg::JSON_INVALID_WEBCHAT;
      result->exit_code = cloud_soul::Err::MsgSend::INVALID_INPUT;
      goal_handle->abort(result);
      return;
    }

    auto msg = std::make_unique<std_msgs::msg::String>();
    msg->data = message;
    web_chat_pub_->publish(std::move(msg));

    result->output_json = cloud_soul::Msg::JSON_STATUS_PUBLISHED;
    result->exit_code = 0;
    goal_handle->succeed(result);
  }

  // ---------- JSON 辅助函数 ----------
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

  // Shell 单引号转义
  static std::string escape_shell(const std::string &s) {
    std::string out;
    for (char c : s) {
      if (c == '\'')
        out += "'\\''";
      else
        out += c;
    }
    return out;
  }

  // 成员变量
  std::string agent_name_;
  std::string topic_output_;

  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr topic_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr web_chat_pub_;
  rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
  rclcpp::TimerBase::SharedPtr publish_timer_;

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