// Copyright (c) leaf
// SPDX-License-Identifier: MIT
//
// =============================================================================
// 前置依赖：使用 email 通知模式前，必须完成以下系统配置，否则邮件发送会失败并返回错误。
//
// 1. 安装 s-nail（支持原生 SMTP，无需本地邮件服务器）
//    sudo apt update && sudo apt install s-nail
//
// 2. 配置发件账号（编辑 ~/.mailrc）
//    以下为 163 邮箱示例，请根据实际邮箱修改：
//
//    set v15-compat
//    set mta=smtp://你的邮箱%40域名:授权码@smtp.163.com
//    set from=你的邮箱@163.com
//
//    注意：
//    - 邮箱中的 @ 必须替换为 %40
//    - 密码部分需使用邮箱生成的授权码，而非登录密码
//    - 其他邮箱（QQ、Gmail 等）请替换对应的 SMTP 服务器地址
//
// 3. 验证配置
//    echo "测试邮件" | s-nail -s "测试主题" 你的收件人@163.com
//    如果能够收到邮件，说明配置成功。
//
// 若未完成上述配置，本节点会在收到 email 通知请求时返回错误（exit_code 非0），
// 不会崩溃，也不会影响其他功能。
// =============================================================================
//
// 节点: user_notify
// 作用: 将 Agent 输出的消息通过邮件等方式通知用户（未来可扩展飞书、微信等）。
//
// 参数:
//   agent_name      - 命名空间前缀，默认 "agent"
//   info_rate       - info 话题发布频率 (Hz)，默认 1.0
//   mail_recipient  - 邮件接收地址，默认 "root@localhost"
//   mail_subject    - 邮件主题前缀，默认 "Cloud-Soul Notification"
//
// 发布/订阅:
//   动作 /<agent_name>/output/user_notify (ExecuteTool)
//     输入 JSON:
//       {
//         "mode": "email",
//         "message": "要通知的内容",
//         "email_recipient": "可选，覆盖默认收件人",
//         "email_subject": "可选，覆盖默认主题"
//       }
//     输出 JSON: {"success": true/false, "mode_used": "email", "error": "..."}
//   话题 /<agent_name>/output/user_notify/info (std_msgs/String) 工具描述 JSON, QoS transient_local
//
// 扩展方法: 在 notify_impl() 的函数指针映射中添加新条目，并实现对应的通知函数。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <thread>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

class UserNotifyNode : public rclcpp::Node {
public:
  UserNotifyNode(const std::string & agent_name, const std::string & tool_name)
      : Node(tool_name), agent_name_(agent_name), tool_name_(tool_name) {
    // 声明参数
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);
    this->declare_parameter<std::string>("mail_recipient", "root@localhost");
    this->declare_parameter<std::string>("mail_subject", "Cloud-Soul Notification");

    double info_rate = this->get_parameter("info_rate").as_double();

    // 动作服务器
    action_server_ = rclcpp_action::create_server<ExecuteTool>(
        this, "/" + agent_name_ + "/output/" + tool_name_,
        [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
        [](auto...) { return rclcpp_action::CancelResponse::ACCEPT; },
        [this](auto goal_handle) {
          std::thread{std::bind(&UserNotifyNode::execute, this,
                                goal_handle)}
              .detach();
        });

    // info 发布者 (Transient Local)
    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
        "/" + agent_name_ + "/output/" + tool_name_ + "/info", qos);

    // 定时发布 info
    publish_timer_ = this->create_wall_timer(
        std::chrono::duration<double>(1.0 / info_rate),
        [this]() { publish_info(); });
    publish_info();

    // 注册可用的通知模式（以后扩展只需在这里添加）
    notify_funcs_["email"] = [this](const NotifyParams &params) -> bool {
      return notify_email(params);
    };

    RCLCPP_INFO(this->get_logger(), "%s 工具节点已启动", tool_name_.c_str());
  }

private:
  // 通知调用所需的统一参数结构（可扩展）
  struct NotifyParams {
    std::string message;
    std::string mode;
    // 邮件相关
    std::string email_recipient;
    std::string email_subject;
  };

  void publish_info() {
    std_msgs::msg::String msg;
    msg.data = R"json(
      {
        "name": "user_notify",
        "description": "向用户发送通知。目前支持邮件方式（未来可扩展飞书、微信等）。注意：使用邮件模式需要系统已安装 s-nail 并正确配置 ~/.mailrc。",
        "parameters": {
          "type": "object",
          "properties": {
            "mode": {
              "type": "string",
              "enum": ["email"],
              "description": "通知方式，目前仅支持 email"
            },
            "message": {
              "type": "string",
              "description": "要通知用户的文本内容"
            },
            "email_recipient": {
              "type": "string",
              "description": "可选，收件人地址，覆盖节点默认收件人"
            },
            "email_subject": {
              "type": "string",
              "description": "可选，邮件主题，覆盖节点默认主题"
            }
          },
          "required": ["mode", "message"]
        }
      }
    )json";
    info_pub_->publish(msg);
  }

  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle) {
    auto result = std::make_shared<ExecuteTool::Result>();
    const auto &input_json = goal_handle->get_goal()->input_json;

    NotifyParams params;
    try {
      params.mode = extract_json_string(input_json, "mode");
      params.message = extract_json_string(input_json, "message");
      params.email_recipient = extract_json_string(input_json, "email_recipient");
      params.email_subject = extract_json_string(input_json, "email_subject");
    } catch (...) {
      result->output_json = R"({"success":false,"error":"invalid json"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    if (params.mode.empty() || params.message.empty()) {
      result->output_json =
          R"({"success":false,"error":"mode or message is empty"})";
      result->exit_code = -2;
      goal_handle->abort(result);
      return;
    }

    // 根据模式调用对应的通知函数
    auto it = notify_funcs_.find(params.mode);
    if (it == notify_funcs_.end()) {
      result->output_json = "{\"success\":false,\"error\":\"unsupported mode\"}";
      result->exit_code = -3;
      goal_handle->abort(result);
      return;
    }

    bool ok = it->second(params);
    std::string output = "{\"success\":" + std::string(ok ? "true" : "false") +
                         ",\"mode_used\":\"" + params.mode + "\"}";
    if (!ok) output = "{\"success\":false,\"mode_used\":\"" + params.mode +
                      "\",\"error\":\"notification failed\"}";
    result->output_json = output;
    result->exit_code = ok ? 0 : -4;
    goal_handle->succeed(result);
  }

  // ---------- 具体通知实现 ----------

  /// @brief 邮件通知，使用 s-nail 命令
  /// @return true 成功发送，false 失败（含 s-nail 命令不可用）
  bool notify_email(const NotifyParams &params) {
    // 检查 s-nail 命令是否存在
    if (system("command -v s-nail > /dev/null 2>&1") != 0) {
      RCLCPP_ERROR(this->get_logger(),
                   "s-nail 命令不可用，请先安装 s-nail 并配置 ~/.mailrc");
      return false;
    }

    // 收件人：优先用请求中的，否则用节点参数
    std::string recipient = params.email_recipient.empty()
                                ? this->get_parameter("mail_recipient").as_string()
                                : params.email_recipient;
    std::string subject = params.email_subject.empty()
                              ? this->get_parameter("mail_subject").as_string()
                              : params.email_subject;

    // 构造 shell 命令，对消息内容进行单引号转义
    std::string escaped_message = escape_shell(params.message);
    std::string cmd = "echo '" + escaped_message + "' | s-nail -s '" +
                      escape_shell(subject) + "' " + recipient;

    RCLCPP_INFO(this->get_logger(), "发送邮件至 %s", recipient.c_str());
    if (system(cmd.c_str()) != 0) {
      RCLCPP_ERROR(this->get_logger(), "邮件发送失败");
      return false;
    }
    return true;
  }

  // 未来扩展示例（桌面通知）
  // bool notify_desktop(const NotifyParams &params) { ... }

  // 未来扩展示例（飞书）
  // bool notify_feishu(const NotifyParams &params) { ... }

  // ---------- 工具函数 ----------

  // 从简单 JSON 中提取字符串值
  static std::string extract_json_string(const std::string &json,
                                          const std::string &key) {
    std::string search = "\"" + key + "\":\"";
    size_t start = json.find(search);
    if (start == std::string::npos)
      return "";
    start += search.length();
    size_t end = start;
    while (end < json.size()) {
      if (json[end] == '"' && (end == 0 || json[end - 1] != '\\'))
        break;
      ++end;
    }
    return json.substr(start, end - start);
  }

  // 转义 shell 单引号字符串
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

  // ---------- 成员变量 ----------
  std::string agent_name_;
  std::string tool_name_;

  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
  rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
  rclcpp::TimerBase::SharedPtr publish_timer_;

  // 通知模式映射表（易于扩展）
  std::map<std::string, std::function<bool(const NotifyParams &)>> notify_funcs_;
};

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  auto temp = std::make_shared<rclcpp::Node>("temp");
  temp->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp->get_parameter("agent_name").as_string();
  temp.reset();

  auto node = std::make_shared<UserNotifyNode>(agent_name, "user_notify");
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}