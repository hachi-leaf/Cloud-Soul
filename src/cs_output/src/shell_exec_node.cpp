// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul 标准 output 工具节点：Shell 命令执行
// shell command execution tool for Cloud-Soul

// Node: /<agent_name>/shell_exec_node
// Param:
//  <string>agent_name       --> Agent 名
//  <float64>info_rate       --> 发布 Tools Info 的频率（Hz）
//  <float64>default_timeout --> 默认状态下（Agent 将 Action 的 Goal 的 timeout_sec 设为 0 时）Action 的 timeout

// Topic: /<agent_name>/output/shell_exec/info
// Struct:
//  <string>info --> 输入给 LLM 的 tools json 字段，见 SHELL_EXEC_INFO_JSON

// Action: /<agent_name>/output/shell_exec
// Struct:
//  Goal <string>input_json      --> LLM 输出的 tools_call json 字段，由 SHELL_EXEC_INFO_JSON 约束
//  Goal <float64>timeout_sec    --> Action 调用超时时间（秒），0 表示使用 default_timeout
//  ---
//  Results <string>output_json  --> 返回给 LLM tools_callback 字段，为自由字符串
//  Results <int32>exit_code     --> 错误码，0 为成功，-1 为错误
//  ---
//  Feedback <string>status      --> reserved

// Action 特性：
//  1. 禁止并行，并行时直接对新 Goal 返回 exit_code = -1, output_json = {"error":"Another goal is already running"}
//  2. 输入校验失败（command 缺失或为空）→ exit_code = -1, output_json = {"error":"invalid input: command is required"}
//  3. 系统错误（临时文件创建/写入、管道/fork 失败）→ exit_code = -1, output_json = {"error":"..."}，具体错误描述见代码
//  4. 命令执行成功（退出码为 0）→ exit_code = 0, output_json = {"stdout":"...","exit_code":0}
//  5. 命令执行失败（退出码非 0）→ exit_code = -1, output_json = {"stdout":"...","exit_code":<实际退出码>}
//  6. 命令执行超时（依据 timeout_sec 或 default_timeout 计算截止时间）→ exit_code = -1, output_json = {"error":"timed out after Xs","stdout":"<已捕获>"}
//  7. 用户主动 Cancel（包括 Ctrl+C 终止节点）→ exit_code = -1, output_json = {"error":"execution canceled","stdout":"<已捕获>"}
//  8. 子进程 stdin 关闭（重定向到 /dev/null），防止交互式命令卡死；Agent 应将交互式命令转为非交互式写法（如 apt install -y，echo password | sudo -S）
//  9. 支持多行命令，通过 /bin/sh 执行临时脚本
// 10. 输入 JSON 格式错误时自动尝试修复（去尾逗号、补全括号等），修复仍失败则返回错误
// 11. 所有执行路径均保证给客户端终结响应（succeed/abort），包括内部异常
// 12. 管道读取设为非阻塞，防止无输出命令导致循环卡死

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
#include <mutex>
#include <cerrno>

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
static constexpr const char* SHELL_EXEC_INFO_JSON = R"json({
  "type": "function",
  "function": {
    "name": "shell_exec",
    "description": "在子进程中执行 Shell 命令，捕获全部 stdout/stderr 并返回。支持多行脚本。命令以非交互方式运行（stdin 已关闭），请确保命令不需要终端输入。对于会产生进度刷新的命令（如 wget、rsync），请使用静默选项抑制进度条，只保留最终结果。\n\n场景示例:\n  - 简单命令: {\"command\":\"ls -la /tmp\"}\n  - 多行脚本: {\"command\":\"for i in 1 2 3; do\\n  echo \\\"Line $i\\\"\\ndone\"}\n  - 管道命令: {\"command\":\"cat /etc/os-release | grep PRETTY\"}\n  - 非交互式 sudo: {\"command\":\"echo 'password' | sudo -S apt update\"}\n  - 非交互式安装: {\"command\":\"apt install -y vim\"}\n  - 下载文件（静默）: {\"command\":\"wget -q -O /tmp/file.zip http://example.com/file.zip\"}\n  - rsync 静默同步: {\"command\":\"rsync -a --quiet /src/ /dst/\"}\n\n规则:\n  - command 不能为空\n  - 命令通过 /bin/sh 执行，stdin 已关闭，任何需要交互输入的命令将立即失败\n  - 需要密码或确认时，请使用非交互式写法，如 echo <password> | sudo -S <cmd> 或 <cmd> -y\n  - 对于会输出大量进度刷新的命令，请使用静默选项（如 wget -q、curl -sS、rsync -q、ffmpeg -nostats -loglevel error）\n  - stdout 和 stderr 合并返回，输出大小无限制\n\n返回值:\n  - 命令成功(退出码0): {\"stdout\":\"...\",\"exit_code\":0}\n  - 命令失败(退出码非0): {\"stdout\":\"...\",\"exit_code\":<实际码>}\n  - 超时: {\"error\":\"timed out after Xs\",\"stdout\":\"...\"}\n  - 任何错误: {\"error\":\"错误原因\"}",
    "parameters": {
      "type": "object",
      "required": ["command"],
      "properties": {
        "command": {
          "type": "string",
          "description": "要执行的 Shell 命令，可包含多行、管道、变量等。请使用非交互式写法，并添加静默选项避免不必要的进度输出。"
        },
        "timeout_sec": {
          "type": "number",
          "description": "命令执行超时时间（秒）。默认 60 秒。设 0 使用默认值。建议长时间任务设置合理超时，避免 Agent 挂起。"
        }
      }
    }
  }
})json";

// 内部常量
static constexpr size_t SHELL_OUTPUT_BUF_SIZE = 4096;
static constexpr std::chrono::milliseconds PROCESS_POLL_INTERVAL(100);

// ---------- JSON 修复函数 ----------
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

class ShellExecNode : public rclcpp::Node {
public:
  ShellExecNode(const std::string & agent_name)
  : Node("shell_exec_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("info_rate", 1.0);
    this->declare_parameter<double>("default_timeout", 60.0);

    double info_rate = this->get_parameter("info_rate").as_double();
    default_timeout_ = this->get_parameter("default_timeout").as_double();

    action_server_ = rclcpp_action::create_server<ExecuteTool>(
      this,
      "/" + agent_name_ + "/output/shell_exec",
      [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
      // handle_cancel
      [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
        std::lock_guard<std::mutex> lock(active_mutex_);
        if (auto it = active_goals_.find(goal_handle->get_goal_id());
            it != active_goals_.end()) {
          it->second->canceled.store(true);
          RCLCPP_INFO(this->get_logger(), "收到取消请求");
        }
        return rclcpp_action::CancelResponse::ACCEPT;
      },
      // handle_accepted
      [this](auto goal_handle) {
        {
          std::lock_guard<std::mutex> lock(active_mutex_);
          if (!active_goals_.empty()) {
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"Another goal is already running"})";
            result->exit_code = -1;
            goal_handle->abort(result);
            RCLCPP_WARN(this->get_logger(), "拒绝新目标：已有命令在执行");
            return;
          }
          auto exec_state = std::make_shared<ExecutionState>();
          exec_state->canceled.store(false);
          active_goals_[goal_handle->get_goal_id()] = exec_state;
        }
        std::thread{std::bind(&ShellExecNode::execute, this, goal_handle,
                              active_goals_[goal_handle->get_goal_id()])}.detach();
      }
    );

    rclcpp::QoS qos(1);
    qos.transient_local();
    qos.reliable();
    info_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/output/shell_exec/info", qos);

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
    msg.data = SHELL_EXEC_INFO_JSON;
    info_pub_->publish(msg);
  }

  void execute(const std::shared_ptr<GoalHandleExecute> goal_handle,
               std::shared_ptr<ExecutionState> exec_state) {
    auto result = std::make_shared<ExecuteTool::Result>();

    try {
      const auto goal = goal_handle->get_goal();

      std::string command;
      try {
        json args = json::parse(goal->input_json);
        if (!args.contains("command") || !args["command"].is_string())
          throw std::runtime_error("missing command");
        command = args["command"].get<std::string>();
        if (command.empty()) throw std::runtime_error("empty command");
      } catch (const json::parse_error &) {
        std::string fixed = repair_json(goal->input_json);
        try {
          json args = json::parse(fixed);
          if (!args.contains("command") || !args["command"].is_string())
            throw std::runtime_error("missing command");
          command = args["command"].get<std::string>();
          if (command.empty()) throw std::runtime_error("empty command");
          RCLCPP_INFO(this->get_logger(), "JSON 自动修复成功");
        } catch (const std::exception &) {
          result->output_json = R"({"error":"invalid input: command is required"})";
          result->exit_code = -1;
          goal_handle->abort(result);
          { std::lock_guard<std::mutex> lock(active_mutex_); active_goals_.erase(goal_handle->get_goal_id()); }
          return;
        }
      } catch (const std::exception &) {
        result->output_json = R"({"error":"invalid input: command is required"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        { std::lock_guard<std::mutex> lock(active_mutex_); active_goals_.erase(goal_handle->get_goal_id()); }
        return;
      }

      double timeout = goal->timeout_sec > 0.0 ? goal->timeout_sec : default_timeout_;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);

      char temp_filename[] = "/tmp/cloudsoul_shell_exec_XXXXXX";
      int temp_fd = mkstemp(temp_filename);
      if (temp_fd == -1) {
        result->output_json = R"({"error":"temporary file creation failed"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        { std::lock_guard<std::mutex> lock(active_mutex_); active_goals_.erase(goal_handle->get_goal_id()); }
        return;
      }

      std::string script = "#!/bin/sh\n" + command;
      if (write(temp_fd, script.c_str(), script.size()) != static_cast<ssize_t>(script.size())) {
        close(temp_fd); unlink(temp_filename);
        result->output_json = R"({"error":"failed to write script to temp file"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        { std::lock_guard<std::mutex> lock(active_mutex_); active_goals_.erase(goal_handle->get_goal_id()); }
        return;
      }
      close(temp_fd);
      chmod(temp_filename, 0700);

      int pipefd[2];
      if (pipe(pipefd) != 0) {
        unlink(temp_filename);
        result->output_json = R"({"error":"pipe creation failed"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        { std::lock_guard<std::mutex> lock(active_mutex_); active_goals_.erase(goal_handle->get_goal_id()); }
        return;
      }

      pid_t pid = fork();
      if (pid == -1) {
        close(pipefd[0]); close(pipefd[1]); unlink(temp_filename);
        result->output_json = R"({"error":"fork failed"})";
        result->exit_code = -1;
        goal_handle->abort(result);
        { std::lock_guard<std::mutex> lock(active_mutex_); active_goals_.erase(goal_handle->get_goal_id()); }
        return;
      }

      if (pid == 0) {
        close(pipefd[0]);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", temp_filename, (char*)nullptr);
        _exit(127);
      }

      close(pipefd[1]);

      // 设置读端为非阻塞，防止 read 在无数据时卡死循环
      int flags = fcntl(pipefd[0], F_GETFL, 0);
      if (flags != -1) {
          fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
      }

      std::string output;
      char buffer[SHELL_OUTPUT_BUF_SIZE];
      int child_exit_code = -1;
      bool canceled = false;
      bool timed_out = false;

      while (true) {
        if (exec_state->canceled.load()) {
          canceled = true;
          kill(pid, SIGKILL);
          struct timespec ws, we;
          clock_gettime(CLOCK_MONOTONIC, &ws);
          while (true) {
            pid_t w = waitpid(pid, NULL, WNOHANG);
            if (w == pid || w == -1) break;
            clock_gettime(CLOCK_MONOTONIC, &we);
            double el = (we.tv_sec - ws.tv_sec) + (we.tv_nsec - ws.tv_nsec) / 1e9;
            if (el > 3.0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          break;
        }

        if (std::chrono::steady_clock::now() > deadline) {
          timed_out = true;
          kill(pid, SIGKILL);
          struct timespec ws, we;
          clock_gettime(CLOCK_MONOTONIC, &ws);
          while (true) {
            pid_t w = waitpid(pid, NULL, WNOHANG);
            if (w == pid || w == -1) break;
            clock_gettime(CLOCK_MONOTONIC, &we);
            double el = (we.tv_sec - ws.tv_sec) + (we.tv_nsec - ws.tv_nsec) / 1e9;
            if (el > 3.0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          break;
        }

        int status;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
          ssize_t n;
          while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            output.append(buffer, n);
          }
          if (WIFEXITED(status)) child_exit_code = WEXITSTATUS(status);
          else if (WIFSIGNALED(status)) child_exit_code = -1;
          break;
        }

        ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
          output.append(buffer, n);
        } else if (n == 0) {
          // EOF, 子进程可能意外退出但 waitpid 尚未捕获？直接 break
          break;
        } else {
          // n < 0
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 非阻塞无数据，正常
          } else {
            // 真正的错误，跳出
            break;
          }
        }

        std::this_thread::sleep_for(PROCESS_POLL_INTERVAL);
      }

      // 确保管道读端关闭
      close(pipefd[0]);
      unlink(temp_filename);

      json out;
      if (canceled) {
        out["error"] = "execution canceled";
        out["stdout"] = output;
        result->exit_code = -1;
      } else if (timed_out) {
        out["error"] = "timed out after " + std::to_string(timeout) + "s";
        out["stdout"] = output;
        result->exit_code = -1;
      } else if (child_exit_code == 0) {
        out["stdout"] = output;
        out["exit_code"] = 0;
        result->exit_code = 0;
      } else {
        out["stdout"] = output;
        out["exit_code"] = child_exit_code;
        result->exit_code = -1;
      }
      result->output_json = out.dump();
      goal_handle->succeed(result);
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

  std::string agent_name_;
  double default_timeout_ = 60.0;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
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
  auto node = std::make_shared<ShellExecNode>(agent_name);
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}