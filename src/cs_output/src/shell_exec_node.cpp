// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// ================================================================
// Cloud-Soul Shell 命令执行工具节点
// ================================================================
//
// 作用:
//   在子进程中执行 Shell 命令，捕获全部 stdout/stderr 并返回。
//   支持多行脚本。命令以非交互方式运行（stdin 已关闭）。
//
// 节点名: /<agent_name>/shell_exec_node
//
// 参数:
//   agent_name       (string, 必填)  Agent 命名空间
//   info_rate         (double, 1.0)  发布 Tools Info 的频率（Hz）
//   default_timeout   (double, 30.0)  命令执行默认超时秒数 (DEFAULT_TIMEOUT)
//
// Action:
//   /<agent_name>/output/shell_exec  (ExecuteTool)
//     Goal: 接收 {"name":"shell_exec","arguments":{"command":"...","timeout_sec":...}}
//     Result: output_json 为自由字符串，透传给 LLM
//     Cancel: 终止正在执行的子进程
//
// 上层传入 JSON 规范 (来自 output_mgmt):
//   output_mgmt 透传以下 JSON 给本节点:
//   {
//     "name": "shell_exec",
//     "arguments": {
//       "command": "ls -la /tmp",         // 必填，要执行的 Shell 命令
//       "timeout_sec": 10                  // 可选，超时秒数，不传则用 default_timeout
//     }
//   }
//
// 关键设计:
//   - 从 arguments 子对象提取参数（与 output_mgmt 接收格式一致）
//   - 无需并行保护（output_mgmt 通过 busy 标志保证同一时间只有一个 Goal）
//   - stdin 关闭（/dev/null），防止交互式命令卡死
//   - 管道读取非阻塞，防止无输出命令卡死循环
//   - 超时/取消均用 SIGKILL 终止子进程
//

// shell_exec_node_pre.cpp

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <cstring>
#include <cerrno>
#include <atomic>
#include <memory>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "nlohmann/json.hpp"

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using json = nlohmann::json;

// 轮询间隔：等待子进程输出时的睡眠周期
static constexpr auto PROCESS_POLL_INTERVAL = std::chrono::milliseconds(100);

// 管道读取缓冲区大小
static constexpr size_t SHELL_OUTPUT_BUF_SIZE = 4096;

// kill 后等待子进程结束的超时
static constexpr auto KILL_WAIT_TIMEOUT = std::chrono::seconds(3);

// kill 后轮询 waitpid 的间隔
static constexpr auto KILL_POLL_INTERVAL = std::chrono::milliseconds(50);

// 命令执行默认超时秒数
static constexpr double DEFAULT_TIMEOUT = 30.0;

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

// ================================================================
// ShellExecNode
// ================================================================
class ShellExecNode : public rclcpp::Node {
public:
    ShellExecNode(const std::string& agent_name)
        : Node("shell_exec_node", agent_name), agent_name_(agent_name)
    {
        // 参数声明
        declare_parameter("agent_name", agent_name);
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", DEFAULT_TIMEOUT);

        default_timeout_ = get_parameter("default_timeout").as_double();

        // Action Server
        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this, "/" + agent_name_ + "/output/shell_exec",
            // handle_goal — 接收 Goal 时触发
            // 无需并行保护（output_mgmt 保证同一时间只有一个 Goal）
            [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
            // handle_cancel — 客户端请求取消时触发
            [this](auto goal_handle) { return handle_cancel(goal_handle); },
            // handle_accepted — Goal 被接受后，启动独立执行线程
            [this](auto goal_handle) { handle_accepted(goal_handle); });

        // Info 发布者 + 定时器
        rclcpp::QoS qos(1);
        qos.transient_local();
        qos.reliable();
        info_pub_ = create_publisher<std_msgs::msg::String>(
            "/" + agent_name_ + "/output/shell_exec/info", qos);

        double info_rate = get_parameter("info_rate").as_double();
        publish_timer_ = create_wall_timer(
            std::chrono::duration<double>(1.0 / info_rate),
            [this]() { publish_info(); });
        publish_info();  // 立即发布一次

        RCLCPP_INFO(get_logger(), "shell_exec_node started");
    }

private:
    // ---------- 发布工具描述 ----------
    void publish_info() {
        std_msgs::msg::String msg;
        msg.data = SHELL_EXEC_INFO_JSON;
        info_pub_->publish(msg);
    }

    // ---------- 处理取消 ----------
    // 设置 canceled_ 标志，由 execute() 轮询检测并 kill 子进程
    rclcpp_action::CancelResponse handle_cancel(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        (void)goal_handle;
        RCLCPP_INFO(get_logger(), "handle_cancel: Cancel requested, signaling execute to stop");
        canceled_.store(true);
        return rclcpp_action::CancelResponse::ACCEPT;
    }

    // ---------- 处理 Goal 接受 ----------
    void handle_accepted(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        const auto goal = goal_handle->get_goal();

        // 解析 JSON，预期格式:
        //  {"name":"shell_exec","arguments":{"command":"...","timeout_sec":...}}
        json input;
        try {
            input = json::parse(goal->input_json);
        } catch (...) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Failed to parse input JSON");
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: failed to parse JSON"})";
            goal_handle->abort(result);
            return;
        }

        // 校验 tool name 是否为 "shell_exec"
        if (!input.contains("name") || !input["name"].is_string() || input["name"].get<std::string>() != "shell_exec") {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Invalid or mismatched tool name");
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: bad tool name"})";
            goal_handle->abort(result);
            return;
        }

        // 校验 arguments 存在且为对象
        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing arguments");
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: missing arguments"})";
            goal_handle->abort(result);
            return;
        }

        // 校验 command 非空字符串
        if (!input["arguments"].contains("command") || !input["arguments"]["command"].is_string() || input["arguments"]["command"].get<std::string>().empty()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing or empty command");
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: command is required"})";
            goal_handle->abort(result);
            return;
        }

        // 启动独立执行线程
        canceled_.store(false);
        std::thread{[this, goal_handle]() { execute(goal_handle); }}.detach();
    }

    // ---------- 核心：执行 Shell 命令 ----------
    void execute(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        auto result = std::make_shared<ExecuteTool::Result>();
        const auto goal = goal_handle->get_goal();

        // 1. 提取参数 (已在 handle_accepted 校验过，安全取值)
        json input = json::parse(goal->input_json);
        std::string command = input["arguments"]["command"].get<std::string>();
        double timeout = default_timeout_;
        if (input["arguments"].contains("timeout_sec") &&
            input["arguments"]["timeout_sec"].is_number()) {
            double ts = input["arguments"]["timeout_sec"].get<double>();
            if (ts > 0.0) timeout = ts;
        }
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::duration<double>(timeout);

        // 2. 创建临时脚本
        char temp_filename[] = "/tmp/cloudsoul_shell_exec_XXXXXX";
        int temp_fd = mkstemp(temp_filename);
        if (temp_fd == -1) {
                        // 退出原因: 系统无法创建临时文件（磁盘满/权限/tmp不可写）
            RCLCPP_ERROR(get_logger(), "execute: Failed to create temp file");
            result->output_json = R"({"error":"temporary file creation failed"})";
            goal_handle->abort(result);
            return;
        }
        std::string script = "#!/bin/sh\n" + command;
        if (write(temp_fd, script.c_str(), script.size()) !=
            static_cast<ssize_t>(script.size())) {
            close(temp_fd); unlink(temp_filename);
                        // 退出原因: 写入临时文件失败（磁盘满或权限问题）
            RCLCPP_ERROR(get_logger(), "execute: Failed to write temp file");
            result->output_json = R"({"error":"failed to write script to temp file"})";
            goal_handle->abort(result);
            return;
        }
        close(temp_fd);
        chmod(temp_filename, 0700);

        // 3. 创建管道
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            unlink(temp_filename);
                        // 退出原因: 管道创建失败（系统 fd 耗尽）
            RCLCPP_ERROR(get_logger(), "execute: Pipe creation failed");
            result->output_json = R"({"error":"pipe creation failed"})";
            goal_handle->abort(result);
            return;
        }

        // 4. fork 子进程
        pid_t pid = fork();
        if (pid == -1) {
            close(pipefd[0]); close(pipefd[1]); unlink(temp_filename);
                        // 退出原因: fork 失败（进程数达上限或内存不足）
            RCLCPP_ERROR(get_logger(), "execute: Fork failed");
            result->output_json = R"({"error":"fork failed"})";
            goal_handle->abort(result);
            return;
        }

        if (pid == 0) {
            // 子进程: stdin→/dev/null, stdout/stderr→管道写端
            close(pipefd[0]);
            int nullfd = open("/dev/null", O_RDONLY);
            if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            execl("/bin/sh", "sh", temp_filename, (char*)nullptr);
            _exit(127);
        }

        // 父进程: 关闭写端，读端设非阻塞
        close(pipefd[1]);
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        if (flags != -1) {
            fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
        }

        // kill 并等待子进程结束，返回 true 表示成功终止
        auto kill_and_wait = [&]() -> bool {
            if (kill(pid, SIGKILL) != 0) {
                RCLCPP_WARN(get_logger(), "execute: kill(%d, SIGKILL) failed, errno=%d", pid, errno);
                return false;
            }
            auto kill_start = std::chrono::steady_clock::now();
            while (true) {
                pid_t w = waitpid(pid, nullptr, WNOHANG);
                if (w == pid || w == -1) return true;
                if (std::chrono::steady_clock::now() - kill_start > KILL_WAIT_TIMEOUT) {
                    RCLCPP_WARN(get_logger(), "execute: kill_and_wait timeout, pid=%d may still be alive", pid);
                    return false;
                }
                std::this_thread::sleep_for(KILL_POLL_INTERVAL);
            }
        };

        // 5. 轮询循环
        std::string output;
        char buffer[SHELL_OUTPUT_BUF_SIZE];
        int child_exit_code = -1;
        bool canceled = false;
        bool timed_out = false;

        while (true) {

            // 检查取消
            if (canceled_.load()) {
                canceled = true;
                kill_and_wait();
                break;
            }

            // 检查超时
            if (std::chrono::steady_clock::now() > deadline) {
                timed_out = true;
                kill_and_wait();
                break;
            }

            // 检查子进程是否已退出
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

            // 读管道输出
            ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
            if (n > 0) {
                output.append(buffer, n);
            } else if (n == 0) {
                break;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            }

            std::this_thread::sleep_for(PROCESS_POLL_INTERVAL);
        }

        // 6. 清理
        close(pipefd[0]);
        unlink(temp_filename);

        // 7. 组装结果
        json out;
        if (canceled) {
            out["error"] = "execution canceled";
            out["stdout"] = output;
                        // 退出原因: 用户通过 output_mgmt 请求取消
            RCLCPP_INFO(get_logger(), "execute: Command canceled, output_size=%zu", output.size());
        } else if (timed_out) {
            out["error"] = "timed out after " + std::to_string(timeout) + "s";
            out["stdout"] = output;
                        // 退出原因: 命令执行超过 timeout_sec 限制
            RCLCPP_WARN(get_logger(), "execute: Command timed out after %.1fs, output_size=%zu",
                        timeout, output.size());
        } else if (child_exit_code == 0) {
            out["stdout"] = output;
            out["exit_code"] = 0;
                        // 正常退出: 命令成功执行，退出码 0
            RCLCPP_INFO(get_logger(), "execute: Command succeeded, exit_code=0, output_size=%zu",
                        output.size());
        } else {
            out["stdout"] = output;
            out["exit_code"] = child_exit_code;
                        // 退出原因: 命令执行完成但返回非零退出码
            RCLCPP_INFO(get_logger(), "execute: Command failed, exit_code=%d, output_size=%zu",
                        child_exit_code, output.size());
        }
        result->output_json = out.dump();
        goal_handle->succeed(result);
    }


    // ========== 成员变量 ==========
    std::string agent_name_;
    double default_timeout_ = DEFAULT_TIMEOUT;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
    rclcpp::TimerBase::SharedPtr publish_timer_;
    std::atomic<bool> canceled_{false};
};

// ================================================================
// main
// ================================================================
int main(int argc, char** argv) {
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
