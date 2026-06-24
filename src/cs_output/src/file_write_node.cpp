// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: /<agent_name>/file_write_node (工具节点，由 output_mgmt_node 自动发现)
// 作用: 将内容写入文件，支持覆盖/追加模式与取消。超时由上层通过取消实现。
//
// 参数:
//   agent_name - 命名空间，默认 "agent"
//   info_rate  - info 话题发布频率(Hz)，默认 1.0
//
// 动作: /<agent_name>/output/file_write_node (ExecuteTool)
//   goal: input_json 包含 {"path": "...", "content": "...", "mode": "overwrite"|"append"}
//   result: output_json 包含 size 或 error
//
// 话题: /<agent_name>/output/file_write_node/info (std_msgs/String) 工具描述 JSON

#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <map>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

class FileWriteNode : public rclcpp::Node {
public:
  FileWriteNode(const std::string & agent_name)
  : Node("file_write_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);
    double info_rate = this->get_parameter("info_rate").as_double();

    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output/file_write_node",
      [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
      [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
        if (auto it = active_goals_.find(goal_handle->get_goal_id());
            it != active_goals_.end()) {
          it->second->canceled.store(true);
          RCLCPP_INFO(this->get_logger(), "收到取消请求");
        }
        return rclcpp_action::CancelResponse::ACCEPT;
      },
      [this](auto goal_handle) {
        auto exec_state = std::make_shared<ExecutionState>();
        exec_state->canceled.store(false);
        active_goals_[goal_handle->get_goal_id()] = exec_state;
        std::thread{std::bind(&FileWriteNode::execute, this, goal_handle, exec_state)}.detach();
      }
    );

    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/file_write_node/info", qos);

    publish_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / info_rate),
      [this]() { publish_info(); });
    publish_info();
  }

private:
  struct ExecutionState {
    std::atomic<bool> canceled;
  };

  void publish_info() {
    std_msgs::msg::String msg;
    msg.data = R"json(
      {
        "name": "file_write_node",
        "description": "将文本内容写入文件，支持覆盖或追加模式。超时由上层管理节点通过取消实现。",
        "parameters": {
          "type": "object",
          "properties": {
            "path": { "type": "string", "description": "文件的绝对路径" },
            "content": { "type": "string", "description": "要写入的文本内容" },
            "mode": {
              "type": "string",
              "enum": ["overwrite", "append"],
              "description": "写入模式，默认 overwrite"
            }
          },
          "required": ["path", "content"]
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

    std::string path, content, mode = "overwrite";
    try {
      path = extract_json_string(input_json, "path");
      content = extract_json_string(input_json, "content");
      std::string mode_tmp = extract_json_string(input_json, "mode");
      if (!mode_tmp.empty()) mode = mode_tmp;
      if (path.empty() || content.empty()) throw std::runtime_error("missing path or content");
    } catch (const std::exception & e) {
      result->output_json = R"EOF({"error":"invalid input json"})EOF";
      result->exit_code = -1;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    std::ios::openmode open_mode = std::ios::binary;
    if (mode == "append") {
      open_mode |= std::ios::app;
    } else {
      open_mode |= std::ios::trunc;
    }

    std::ofstream file(path, open_mode);
    if (!file.is_open()) {
      result->output_json = R"EOF({"error":"failed to open file"})EOF";
      result->exit_code = -2;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    constexpr size_t CHUNK_SIZE = 4096;
    size_t total_written = 0;
    size_t content_len = content.size();
    bool canceled = false;

    while (total_written < content_len) {
      if (exec_state->canceled.load()) {
        canceled = true;
        break;
      }

      size_t to_write = std::min(CHUNK_SIZE, content_len - total_written);
      file.write(content.data() + total_written, to_write);
      if (file.fail()) {
        file.close();
        result->output_json = R"EOF({"error":"write failed"})EOF";
        result->exit_code = -3;
        goal_handle->abort(result);
        active_goals_.erase(goal_handle->get_goal_id());
        return;
      }
      total_written += to_write;
    }

    file.close();

    if (canceled) {
      result->output_json = R"({"error":"execution canceled","size":)" + std::to_string(total_written) + "}";
      result->exit_code = -7;
    } else {
      result->output_json = "{\"size\":" + std::to_string(total_written) + "}";
      result->exit_code = 0;
    }

    goal_handle->succeed(result);
    active_goals_.erase(goal_handle->get_goal_id());
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

  static std::string escape_json_string(const std::string & input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
      switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            out += "\\u00";
            out += "0123456789abcdef"[(c >> 4) & 0xf];
            out += "0123456789abcdef"[c & 0xf];
          } else {
            out += c;
          }
      }
    }
    return out;
  }

  std::string agent_name_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
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
  auto node = std::make_shared<FileWriteNode>(agent_name);
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}