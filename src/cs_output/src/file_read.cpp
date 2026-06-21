// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: file_read
// 作用: 读取指定文本文件，返回内容及行数。
//
// 参数:
//   agent_name - 命名空间前缀，默认 "agent"
//   info_rate  - info 话题发布频率 (Hz)，默认 1.0
//
// 发布/订阅:
//   动作 /<agent_name>/output/file_read (ExecuteTool) 输入 {"path", "max_lines"}, 输出 {"content", "line_count"}
//   话题 /<agent_name>/output/file_read/info (std_msgs/String) 工具描述 JSON, QoS transient_local
//
// Note: 文件读取上限 200 行；内容中的特殊字符会做 JSON 转义。

#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

class FileReadNode : public rclcpp::Node {
public:
  FileReadNode(const std::string & agent_name, const std::string & tool_name)
  : Node(tool_name), agent_name_(agent_name), tool_name_(tool_name)
  {
    // 参数声明 (agent_name 已由 main 传递，此处保留以便节点独立运行时获取)
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);

    double info_rate = this->get_parameter("info_rate").as_double();

    // 动作服务器
    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output/" + tool_name_,
      [this](const rclcpp_action::GoalUUID &, std::shared_ptr<const ExecuteTool::Goal>) {
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
      },
      [this](const std::shared_ptr<GoalHandleExecute>) {
        return rclcpp_action::CancelResponse::ACCEPT;
      },
      [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
        std::thread{std::bind(&FileReadNode::execute, this, goal_handle)}.detach();
      }
    );

    // info 发布者 (Transient Local)
    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/" + tool_name_ + "/info", qos);

    // 定时发布 info
    publish_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / info_rate),
      [this]() { this->publish_info(); }
    );

    publish_info();  // 立即发布一次
    RCLCPP_INFO(this->get_logger(), "%s 工具节点已启动", tool_name_.c_str());
  }

private:
  void publish_info() {
    std_msgs::msg::String msg;
    msg.data = R"(
      {
        "name": "file_read",
        "description": "读取指定文本文件的内容。可以指定最大返回行数，避免输出过长。",
        "parameters": {
          "type": "object",
          "properties": {
            "path": {
              "type": "string",
              "description": "要读取的文件的绝对路径"
            },
            "max_lines": {
              "type": "integer",
              "description": "最多返回的行数，默认 200",
              "default": 200
            }
          },
          "required": ["path"]
        }
      }
    )";
    // 去除多余的空白以便保持 JSON 紧凑（可选）
    info_pub_->publish(msg);
  }

  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle) {
    auto result = std::make_shared<ExecuteTool::Result>();
    const auto goal = goal_handle->get_goal();
    const std::string & input_json = goal->input_json;

    std::string path;
    int max_lines = 200;

    // 简易 JSON 解析 (仅提取两个字段)
    try {
      path = extract_json_string(input_json, "path");
      std::string max_lines_str = extract_json_value(input_json, "max_lines");
      if (!max_lines_str.empty()) {
        max_lines = std::stoi(max_lines_str);
      }
    } catch (const std::exception & e) {
      result->output_json = R"({"error":"invalid input json format"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    if (path.empty()) {
      result->output_json = R"({"error":"missing 'path' parameter"})";
      result->exit_code = -2;
      goal_handle->abort(result);
      return;
    }

    // 打开文件
    std::ifstream file(path);
    if (!file.is_open()) {
      result->output_json = R"({"error":"cannot open file"})";
      result->exit_code = -3;
      goal_handle->abort(result);
      return;
    }

    // 逐行读取，最多 max_lines 行
    std::stringstream ss;
    std::string line;
    int line_count = 0;
    while (std::getline(file, line) && line_count < max_lines) {
      ss << line << "\n";
      ++line_count;
    }
    file.close();

    // 构造输出 JSON (转义换行符和引号)
    std::string content = ss.str();
    // 简单的 JSON 转义
    std::string escaped_content;
    for (char c : content) {
      if (c == '\\') escaped_content += "\\\\";
      else if (c == '"') escaped_content += "\\\"";
      else if (c == '\n') escaped_content += "\\n";
      else if (c == '\r') escaped_content += "\\r";
      else if (c == '\t') escaped_content += "\\t";
      else escaped_content += c;
    }

    std::string output = "{\"content\":\"" + escaped_content + "\",\"line_count\":" + std::to_string(line_count) + "}";
    result->output_json = output;
    result->exit_code = 0;
    goal_handle->succeed(result);
  }

  // 从 JSON 字符串中提取字符串值 (假设 key 后跟 ":"string")
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
    return json.substr(start, end - start);
  }

  // 提取任意 JSON 值 (字符串、数字等)，不处理嵌套对象
  static std::string extract_json_value(const std::string & json, const std::string & key) {
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start == std::string::npos) return "";
    start += search.length();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    if (start >= json.size()) return "";
    if (json[start] == '"') {
      // 字符串值
      return extract_json_string(json, key);  // 重新利用
    } else {
      // 数字或布尔，找到逗号或 } 结束
      size_t end = start;
      while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ' ') ++end;
      return json.substr(start, end - start);
    }
  }

  std::string agent_name_;
  std::string tool_name_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
  rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
  rclcpp::TimerBase::SharedPtr publish_timer_;
};

int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);

  // 获取 agent_name 参数
  auto temp_node = std::make_shared<rclcpp::Node>("temp");
  temp_node->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp_node->get_parameter("agent_name").as_string();
  temp_node.reset();

  auto node = std::make_shared<FileReadNode>(agent_name, "file_read");
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}