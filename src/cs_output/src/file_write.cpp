// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: file_write
// 作用: 将文本写入文件，支持覆盖或追加模式。
//
// 参数:
//   agent_name - 命名空间前缀，默认 "agent"
//   info_rate  - info 话题发布频率 (Hz)，默认 1.0
//
// 发布/订阅:
//   动作 /<agent_name>/output/file_write (ExecuteTool) 输入 {"path", "content", "mode"}, 输出 {"success", "bytes_written"}
//   话题 /<agent_name>/output/file_write/info (std_msgs/String) 工具描述 JSON, QoS transient_local
//
// Note: 自动创建父目录；mode 为 "overwrite" 或 "append"，默认覆盖。

#include <fstream>
#include <sstream>
#include <thread>
#include <sys/stat.h>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

class FileWriteNode : public rclcpp::Node {
public:
  FileWriteNode(const std::string & agent_name, const std::string & tool_name)
  : Node(tool_name), agent_name_(agent_name), tool_name_(tool_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);
    double info_rate = this->get_parameter("info_rate").as_double();

    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output/" + tool_name_,
      [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
      [](auto...) { return rclcpp_action::CancelResponse::ACCEPT; },
      [this](auto goal_handle) {
        std::thread{std::bind(&FileWriteNode::execute, this, goal_handle)}.detach();
      }
    );

    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/" + tool_name_ + "/info", qos);

    publish_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / info_rate),
      [this]() { publish_info(); });
    publish_info();
  }

private:
  void publish_info() {
    std_msgs::msg::String msg;
    msg.data = R"(
      {
        "name": "file_write",
        "description": "将文本内容写入文件。可选择覆盖或追加模式。",
        "parameters": {
          "type": "object",
          "properties": {
            "path": {
              "type": "string",
              "description": "目标文件的绝对路径"
            },
            "content": {
              "type": "string",
              "description": "要写入的文本内容"
            },
            "mode": {
              "type": "string",
              "enum": ["overwrite", "append"],
              "description": "写入模式，overwrite 会覆盖原文件，append 追加到末尾。默认 overwrite",
              "default": "overwrite"
            }
          },
          "required": ["path", "content"]
        }
      }
    )";
    info_pub_->publish(msg);
  }

  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle) {
    auto result = std::make_shared<ExecuteTool::Result>();
    const auto & input_json = goal_handle->get_goal()->input_json;

    std::string path, content, mode = "overwrite";
    try {
      path = extract_json_string(input_json, "path");
      content = extract_json_string(input_json, "content");
      std::string mode_tmp = extract_json_string(input_json, "mode");
      if (!mode_tmp.empty()) mode = mode_tmp;
    } catch (const std::exception & e) {
      result->output_json = R"({"error":"invalid input json"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    if (path.empty() || content.empty()) {
      result->output_json = R"({"error":"path or content missing"})";
      result->exit_code = -2;
      goal_handle->abort(result);
      return;
    }

    // 确保父目录存在 (仅当目录不存在时尝试创建)
    size_t last_slash = path.find_last_of('/');
    if (last_slash != std::string::npos) {
      std::string dir = path.substr(0, last_slash);
      struct stat st;
      if (stat(dir.c_str(), &st) != 0) {
        // 目录不存在，递归创建
        std::string cmd = "mkdir -p \"" + dir + "\"";
        if (system(cmd.c_str()) != 0) {
          result->output_json = R"({"error":"failed to create parent directory"})";
          result->exit_code = -3;
          goal_handle->abort(result);
          return;
        }
      }
    }

    // 打开文件
    std::ios_base::openmode open_mode = (mode == "append") ? std::ios::app : std::ios::trunc;
    std::ofstream file(path, open_mode);
    if (!file.is_open()) {
      result->output_json = R"({"error":"cannot open file for writing"})";
      result->exit_code = -4;
      goal_handle->abort(result);
      return;
    }

    file << content;
    file.close();

    if (file.fail()) {
      result->output_json = R"({"error":"write operation failed"})";
      result->exit_code = -5;
      goal_handle->abort(result);
      return;
    }

    std::string output = "{\"success\":true,\"bytes_written\":" + std::to_string(content.size()) + "}";
    result->output_json = output;
    result->exit_code = 0;
    goal_handle->succeed(result);
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
    return json.substr(start, end - start);
  }

  std::string agent_name_;
  std::string tool_name_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
  rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
  rclcpp::TimerBase::SharedPtr publish_timer_;
};

int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);
  auto temp = std::make_shared<rclcpp::Node>("temp");
  temp->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp->get_parameter("agent_name").as_string();
  temp.reset();

  auto node = std::make_shared<FileWriteNode>(agent_name, "file_write");
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}