// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: shell_exec
// 作用: 在子进程中执行 shell 命令，捕获全部 stdout/stderr 并返回。
//
// 参数:
//   agent_name - 命名空间前缀，默认 "agent"
//   info_rate  - info 话题发布频率 (Hz)，默认 1.0
//
// 发布/订阅:
//   动作 /<agent_name>/output/shell_exec (ExecuteTool) 输入 {"command", "timeout_ms"}, 输出 {"stdout", "exit_code"} 或超时错误
//   话题 /<agent_name>/output/shell_exec/info (std_msgs/String) 工具描述 JSON, QoS transient_local
//
// Note: 支持多行命令，整体写入临时脚本一次性执行；超时后强制终止并返回错误信息。

#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

class ShellExecNode : public rclcpp::Node {
public:
  ShellExecNode(const std::string & agent_name, const std::string & tool_name)
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
        std::thread{std::bind(&ShellExecNode::execute, this, goal_handle)}.detach();
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
    msg.data = R"json(
      {
        "name": "shell_exec",
        "description": "在子进程中执行 shell 命令。支持多行脚本，一次性执行并返回全部终端输出。",
        "parameters": {
          "type": "object",
          "properties": {
            "command": {
              "type": "string",
              "description": "要执行的 shell 命令，可以包含多行，会被整体写入临时脚本执行"
            },
            "timeout_ms": {
              "type": "integer",
              "description": "超时时间（毫秒），超过则终止进程。默认 10000",
              "default": 10000
            }
          },
          "required": ["command"]
        }
      }
    )json";
    info_pub_->publish(msg);
  }

  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle) {
    auto result = std::make_shared<ExecuteTool::Result>();
    const auto & input_json = goal_handle->get_goal()->input_json;

    std::string command;
    int timeout_ms = 10000;

    try {
      command = extract_json_string(input_json, "command");
      std::string timeout_str = extract_json_value(input_json, "timeout_ms");
      if (!timeout_str.empty()) {
        timeout_ms = std::stoi(timeout_str);
      }
    } catch (const std::exception & e) {
      result->output_json = R"({"error":"invalid input json"})";
      result->exit_code = -1;
      goal_handle->abort(result);
      return;
    }

    if (command.empty()) {
      result->output_json = R"({"error":"missing command"})";
      result->exit_code = -2;
      goal_handle->abort(result);
      return;
    }

    // 创建临时脚本文件
    char temp_filename[] = "/tmp/cloudsoul_shell_exec_XXXXXX";
    int temp_fd = mkstemp(temp_filename);
    if (temp_fd == -1) {
      result->output_json = R"({"error":"failed to create temp file"})";
      result->exit_code = -3;
      goal_handle->abort(result);
      return;
    }

    std::string script = "#!/bin/sh\n" + command;
    if (write(temp_fd, script.c_str(), script.size()) != static_cast<ssize_t>(script.size())) {
      close(temp_fd);
      unlink(temp_filename);
      result->output_json = R"({"error":"failed to write temp script"})";
      result->exit_code = -4;
      goal_handle->abort(result);
      return;
    }
    close(temp_fd);

    // 确保可执行
    chmod(temp_filename, 0700);

    // 执行脚本并捕获输出
    std::string full_output;
    int exit_code = -1;
    bool timed_out = false;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
      unlink(temp_filename);
      result->output_json = R"({"error":"pipe creation failed"})";
      result->exit_code = -5;
      goal_handle->abort(result);
      return;
    }

    pid_t pid = fork();
    if (pid == -1) {
      close(pipefd[0]); close(pipefd[1]);
      unlink(temp_filename);
      result->output_json = R"({"error":"fork failed"})";
      result->exit_code = -6;
      goal_handle->abort(result);
      return;
    }

    if (pid == 0) {
      // 子进程
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
      execl("/bin/sh", "sh", temp_filename, (char*)nullptr);
      _exit(127);
    } else {
      // 父进程
      close(pipefd[1]);

      std::stringstream output_ss;
      char buffer[256];
      auto start_time = std::chrono::steady_clock::now();

      while (true) {
        int status;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
          while (true) {
            ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
            if (count > 0) {
              output_ss.write(buffer, count);
            } else break;
          }
          if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
          else exit_code = -1;
          break;
        }

        ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
        if (count > 0) {
          output_ss.write(buffer, count);
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        if (elapsed >= timeout_ms) {
          timed_out = true;
          kill(pid, SIGKILL);
          waitpid(pid, nullptr, 0);
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }

      close(pipefd[0]);
      full_output = output_ss.str();
    }

    unlink(temp_filename);

    // 构造输出 JSON
    std::string escaped_output;
    for (char c : full_output) {
      if (c == '\\') escaped_output += "\\\\";
      else if (c == '"') escaped_output += "\\\"";
      else if (c == '\n') escaped_output += "\\n";
      else if (c == '\r') escaped_output += "\\r";
      else if (c == '\t') escaped_output += "\\t";
      else if (static_cast<unsigned char>(c) < 0x20) {
        escaped_output += "\\u00";
        escaped_output += "0123456789abcdef"[(c >> 4) & 0xf];
        escaped_output += "0123456789abcdef"[c & 0xf];
      }
      else escaped_output += c;
    }

    std::string output_json;
    if (timed_out) {
      output_json = R"({"error":"execution timed out","stdout":")" + escaped_output + R"("})";
      result->output_json = output_json;
      result->exit_code = -7;
    } else {
      output_json = "{\"stdout\":\"" + escaped_output + "\",\"exit_code\":" + std::to_string(exit_code) + "}";
      result->output_json = output_json;
      result->exit_code = (exit_code == 0) ? 0 : -8;
    }

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

  auto node = std::make_shared<ShellExecNode>(agent_name, "shell_exec");
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}