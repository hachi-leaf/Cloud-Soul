// Copyright (c) 2026 Leaf
// SPDX-License-Identifier: MIT

// 节点: /<agent_name>/file_read_node (工具节点，由 output_mgmt_node 自动发现)
// 作用: 读取指定文件内容，支持分块读取与取消。超时由上层通过取消实现。
//
// 参数:
//   agent_name - 命名空间，默认 "agent"
//   info_rate  - info 话题发布频率(Hz)，默认 1.0
//
// 动作: /<agent_name>/output/file_read_node (ExecuteTool)
//   goal: input_json 包含 {"path": "...", "encoding": "utf-8", "offset": 0, "length": -1}
//   result: output_json 包含 content/size 或 error
//
// 话题: /<agent_name>/output/file_read_node/info (std_msgs/String) 工具描述 JSON

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

class FileReadNode : public rclcpp::Node {
public:
  FileReadNode(const std::string & agent_name)
  : Node("file_read_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);
    double info_rate = this->get_parameter("info_rate").as_double();

    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output/file_read_node",
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
        std::thread{std::bind(&FileReadNode::execute, this, goal_handle, exec_state)}.detach();
      }
    );

    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/file_read_node/info", qos);

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
        "name": "file_read_node",
        "description": "读取文件内容，支持指定编码、偏移和长度。超时由上层管理节点通过取消实现。",
        "parameters": {
          "type": "object",
          "properties": {
            "path": { "type": "string", "description": "文件的绝对路径" },
            "encoding": { "type": "string", "description": "编码，默认 utf-8" },
            "offset": { "type": "integer", "description": "起始字节偏移，默认 0" },
            "length": { "type": "integer", "description": "最大读取字节数，-1 表示全部" }
          },
          "required": ["path"]
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

    std::string path, encoding = "utf-8";
    long offset = 0, length = -1;
    try {
      path = extract_json_string(input_json, "path");
      if (path.empty()) throw std::runtime_error("missing path");
      std::string enc = extract_json_string(input_json, "encoding");
      if (!enc.empty()) encoding = enc;
      std::string off_str = extract_json_value(input_json, "offset");
      if (!off_str.empty()) offset = std::stol(off_str);
      std::string len_str = extract_json_value(input_json, "length");
      if (!len_str.empty()) length = std::stol(len_str);
    } catch (const std::exception & e) {
      result->output_json = R"EOF({"error":"invalid input json"})EOF";
      result->exit_code = -1;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      result->output_json = R"EOF({"error":"failed to open file"})EOF";
      result->exit_code = -2;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    if (offset > 0) {
      file.seekg(offset, std::ios::beg);
      if (file.fail()) {
        result->output_json = R"EOF({"error":"seek failed"})EOF";
        result->exit_code = -3;
        goal_handle->abort(result);
        active_goals_.erase(goal_handle->get_goal_id());
        return;
      }
    }

    constexpr size_t CHUNK_SIZE = 4096;
    std::vector<char> buffer(CHUNK_SIZE);
    std::string content;
    size_t total_read = 0;
    bool canceled = false;

    while (!file.eof()) {
      if (exec_state->canceled.load()) {
        canceled = true;
        break;
      }

      size_t to_read = CHUNK_SIZE;
      if (length >= 0 && (total_read + to_read) > static_cast<size_t>(length)) {
        to_read = static_cast<size_t>(length) - total_read;
      }
      if (to_read == 0) break;

      file.read(buffer.data(), to_read);
      size_t bytes_read = file.gcount();
      if (bytes_read == 0) break;
      content.append(buffer.data(), bytes_read);
      total_read += bytes_read;
    }

    file.close();
    std::string escaped_content = escape_json_string(content);

    if (canceled) {
      result->output_json = R"({"error":"execution canceled","content":")" + escaped_content + R"(","size":)" + std::to_string(total_read) + "}";
      result->exit_code = -7;
    } else {
      result->output_json = "{\"content\":\"" + escaped_content + "\",\"size\":" + std::to_string(total_read) + "}";
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

  static std::string extract_json_value(const std::string & json, const std::string & key) {
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start == std::string::npos) return "";
    start += search.length();
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    if (start >= json.size()) return "";
    if (json[start] == '"') {
      return extract_json_string(json, key);
    } else {
      size_t end = start;
      while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
      return json.substr(start, end - start);
    }
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
  auto node = std::make_shared<FileReadNode>(agent_name);
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}