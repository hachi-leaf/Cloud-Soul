// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: /<agent_name>/shell_exec_node (工具节点，由 output_mgmt_node 自动发现)
// 作用: 在子进程中执行 shell 命令，捕获全部 stdout/stderr 并返回。
//       超时由上层 output_mgmt_node 通过动作取消实现，本节点不自行超时。
//
// 参数:
//   agent_name - 命名空间，默认 "agent"
//   info_rate  - info 话题发布频率(Hz)，默认 1.0
//
// 动作: /<agent_name>/output/shell_exec_node (ExecuteTool)
//   goal: input_json 包含 {"command": "..."}
//   result: output_json 包含 stdout/exit_code 或 error
//   支持取消：取消时强制终止子进程并返回错误信息
//
// 话题: /<agent_name>/output/shell_exec_node/info (std_msgs/String) 工具描述 JSON

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
#include <atomic>
#include <map>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "cs_interfaces/constants.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

class ShellExecNode : public rclcpp::Node {
public:
  ShellExecNode(const std::string & agent_name)
  : Node("shell_exec_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);

    double info_rate = this->get_parameter("info_rate").as_double();

    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output/shell_exec_node",
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
        std::thread{std::bind(&ShellExecNode::execute, this, goal_handle, exec_state)}.detach();
      }
    );

    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/shell_exec_node/info", qos);

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
        "name": "shell_exec_node",
        "description": "在子进程中执行 shell 命令。支持多行脚本，一次性执行并返回全部终端输出。超时由上层管理节点通过取消实现。",
        "parameters": {
          "type": "object",
          "properties": {
            "command": {
              "type": "string",
              "description": "要执行的 shell 命令，可以包含多行"
            }
          },
          "required": ["command"]
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

    std::string command;
    try {
      command = extract_json_string(input_json, "command");
      if (command.empty()) throw std::runtime_error("missing command");
    } catch (const std::exception & e) {
      result->output_json = cloud_soul::Msg::SHELL_JSON_INVALID_INPUT;
      result->exit_code = cloud_soul::Err::ShellExec::INVALID_INPUT;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    // 创建临时脚本
    char temp_filename[] = "/tmp/cloudsoul_shell_exec_XXXXXX";  // cloud_soul::SHELL_TEMP_TEMPLATE
    int temp_fd = mkstemp(temp_filename);
    if (temp_fd == -1) {
      result->output_json = cloud_soul::Msg::SHELL_JSON_TEMP_FAIL;
      result->exit_code = cloud_soul::Err::ShellExec::TEMP_FILE_FAIL;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    std::string script = "#!/bin/sh\n" + command;
    if (write(temp_fd, script.c_str(), script.size()) != static_cast<ssize_t>(script.size())) {
      close(temp_fd);
      unlink(temp_filename);
      result->output_json = cloud_soul::Msg::SHELL_JSON_WRITE_FAIL;
      result->exit_code = cloud_soul::Err::ShellExec::TEMP_WRITE_FAIL;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }
    close(temp_fd);
    chmod(temp_filename, 0700);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
      unlink(temp_filename);
      result->output_json = cloud_soul::Msg::SHELL_JSON_PIPE_FAIL;
      result->exit_code = cloud_soul::Err::ShellExec::PIPE_FAIL;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    pid_t pid = fork();
    if (pid == -1) {
      close(pipefd[0]); close(pipefd[1]);
      unlink(temp_filename);
      result->output_json = cloud_soul::Msg::SHELL_JSON_FORK_FAIL;
      result->exit_code = cloud_soul::Err::ShellExec::FORK_FAIL;
      goal_handle->abort(result);
      active_goals_.erase(goal_handle->get_goal_id());
      return;
    }

    if (pid == 0) {
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
      execl("/bin/sh", "sh", temp_filename, (char*)nullptr);
      _exit(127);
    }

    close(pipefd[1]);
    std::stringstream output_ss;
    char buffer[cloud_soul::SHELL_OUTPUT_BUF_SIZE];
    int child_exit_code = -1;
    bool canceled = false;

    while (true) {
      if (exec_state->canceled.load()) {
        canceled = true;
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        break;
      }

      int status;
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        while (true) {
          ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
          if (count > 0) output_ss.write(buffer, count);
          else break;
        }
        if (WIFEXITED(status)) child_exit_code = WEXITSTATUS(status);
        else child_exit_code = -1;
        break;
      }

      ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
      if (count > 0) output_ss.write(buffer, count);

      std::this_thread::sleep_for(cloud_soul::PROCESS_POLL_INTERVAL);
    }

    close(pipefd[0]);
    unlink(temp_filename);

    std::string escaped_output = escape_json_string(output_ss.str());

    if (canceled) {
      result->output_json = R"({"error":"execution canceled","stdout":")" + escaped_output + R"("})";
      result->exit_code = cloud_soul::Err::ShellExec::CANCELED;
    } else {
      result->output_json = "{\"stdout\":\"" + escaped_output + "\",\"exit_code\":" + std::to_string(child_exit_code) + "}";
      result->exit_code = (child_exit_code == 0) ? cloud_soul::Err::ShellExec::OK : cloud_soul::Err::ShellExec::NONZERO_EXIT;
    }

    goal_handle->succeed(result);
    active_goals_.erase(goal_handle->get_goal_id());
  }

  // ---------- JSON 辅助函数（保持不变）----------
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

  auto node = std::make_shared<ShellExecNode>(agent_name);
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}