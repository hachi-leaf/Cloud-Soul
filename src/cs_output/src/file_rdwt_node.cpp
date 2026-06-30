// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// ================================================================
// Cloud-Soul 文件读写工具节点
// ================================================================
//
// 作用:
//   安全的文件读写工具。支持读取（可按行范围）、覆盖写入、追加写入、
//   按行插入、写后读回验证。
//
// 节点名: /<agent_name>/file_rdwt_node
//
// 参数:
//   agent_name       (string, 必填)   Agent 命名空间
//   info_rate         (double, 1.0)   发布 Tools Info 的频率（Hz）
//   default_timeout   (double, 30.0)  文件操作默认超时秒数 (DEFAULT_TIMEOUT)
//
// Action:
//   /<agent_name>/output/file_rdwt  (ExecuteTool)
//     Goal: 接收 {"name":"file_rdwt","arguments":{"action":...,"path":...}}
//     Result: output_json 为自由字符串
//     Cancel: 终止正在进行的读写操作
//
// 上层传入 JSON 规范 (来自 output_mgmt):
//   {
//     "name": "file_rdwt",
//     "arguments": {
//       "action": "read|write|read_write",
//       "path": "/absolute/path",
//       "content": "...",       // write/read_write 必填
//       "mode": "overwrite|append|insert",
//       "range": {"start_line":1,"end_line":10}
//     }
//   }
//
// 关键设计:
//   - 从 arguments 子对象提取参数
//   - 无需并行保护（output_mgmt 通过 busy 标志保证）
//   - 分块读写，每块检查 cancel/timeout 以实现可中断
//   - path 必须是绝对路径，不含 ..
//

// file_rdwt_node_pre.cpp

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "nlohmann/json.hpp"

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using json = nlohmann::json;

static constexpr const char* FILE_RDWT_INFO_JSON = R"json({
  "type": "function",
  "function": {
    "name": "file_rdwt",
    "description": "安全的文件读写工具。支持读取（可按行范围）、覆盖写入、追加写入、按行插入、写后读回验证。\n\n场景示例:\n  - 读全文: {\"action\":\"read\",\"path\":\"/tmp/note.txt\"}\n  - 读第5-10行: {\"action\":\"read\",\"path\":\"/tmp/data.csv\",\"range\":{\"start_line\":5,\"end_line\":10}}\n  - 读到末尾: end_line 设为 -1\n  - 覆盖写入: {\"action\":\"write\",\"path\":\"/tmp/foo.txt\",\"content\":\"hello\"} (mode 默认 overwrite)\n  - 追加到末尾: mode=\"append\"\n  - 在第3行前插入: {\"action\":\"write\",\"path\":\"/tmp/code.py\",\"content\":\"import os\\n\",\"mode\":\"insert\",\"range\":{\"start_line\":3}}\n  - 先写后读回: {\"action\":\"read_write\",\"path\":\"/tmp/config.json\",\"content\":\"{}\",\"mode\":\"overwrite\"}\n\n规则:\n  - path 必须是绝对路径（以 / 开头），支持空格，不能包含 ..\n  - path 是目录会报错；read 不存在的文件报错，write 不存在的文件自动创建\n  - content 不能为空串（否则报错）\n  - range 为 1-indexed；start_line 必须 ≥1；end_line=-1 表示到末尾；end<start 报错\n  - range 越界自动 clamp，不报错（如只有5行但读4-100行 → 返回第4-5行）\n  - insert 必须带 range.start_line；行号超出文件行数时等效 append；不带 range 报错\n  - append 到不存在的文件自动创建（等效 overwrite）\n\n返回值:\n  - read 成功: {\"exit_code\":0,\"content\":\"...\",\"size\":123}\n  - write 成功: {\"exit_code\":0,\"written\":456}\n  - read_write 成功: {\"exit_code\":0,\"content\":\"...\",\"size\":456,\"written\":456}\n  - 任何错误: {\"exit_code\":-1,\"error\":\"错误原因\"}",
    "parameters": {
      "type": "object",
      "required": ["action", "path"],
      "properties": {
        "action": {
          "type": "string",
          "enum": ["read", "write", "read_write"],
          "description": "read=读取, write=写入, read_write=先写入再读回全文"
        },
        "path": {
          "type": "string",
          "description": "文件绝对路径，如 /tmp/foo.txt。支持空格，不支持 .."
        },
        "content": {
          "type": "string",
          "description": "写入内容。action=write 或 read_write 时必填，不能为空串"
        },
        "mode": {
          "type": "string",
          "enum": ["overwrite", "append", "insert"],
          "default": "overwrite",
          "description": "写入模式。append 到不存在的文件自动创建。insert 需配合 range.start_line"
        },
        "range": {
          "type": "object",
          "description": "行范围（1-indexed）。read 时限定读取行；insert 时指定插入位置。越界自动 clamp",
          "properties": {
            "start_line": {
              "type": "integer",
              "minimum": 1,
              "description": "起始行号（≥1）"
            },
            "end_line": {
              "type": "integer",
              "description": "结束行号。设为 -1 表示读到末尾。必须 ≥ start_line"
            }
          }
        },
        "timeout_sec": {
          "type": "number",
          "description": "文件操作超时时间（秒）。默认 60 秒。设 0 使用默认值。建议长时间任务设置合理超时，避免 Agent 挂起。"
        }
      }
    }
  }
})json";

static json make_error(const std::string& msg) {
    return {{"error", msg}};
}

// ================================================================
// JSON repair: defensive fix for common LLM output issues
// ================================================================
static std::string repair_json(const std::string& raw) {
    std::string s = raw;
    size_t p0 = s.find_first_not_of(" \\t\\n\\r");



    size_t p1 = s.find_last_not_of(" \\t\\n\\r");



    if (p0 == std::string::npos) return s;
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

// Line-based operations (1-indexed, \n delimiter)
// ================================================================
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) lines.push_back(line);
    // discard trailing empty from final \n
    return lines;
}

static std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += "\n";
        out += lines[i];
    }
    if (!lines.empty()) out += "\n";
    return out;
}

static std::string read_lines_range(const std::string& content,
                                     int64_t start, int64_t end) {
    auto lines = split_lines(content);
    int64_t total = static_cast<int64_t>(lines.size());
    if (total == 0) return "";
    // clamp
    if (start < 1) start = 1;
    if (end < 0 || end > total) end = total;
    if (start > total) return "";
    if (start > end) return "";
    std::vector<std::string> slice(
        lines.begin() + (start - 1),
        lines.begin() + end);
    return join_lines(slice);
}

static std::string content_insert(const std::string& original,
                                    const std::string& insert_content,
                                    int64_t at_line) {
    auto lines = split_lines(original);
    auto ins   = split_lines(insert_content);
    int64_t total = static_cast<int64_t>(lines.size());

    if (total == 0 || at_line > total) {
        // degenerate to append
        lines.insert(lines.end(), ins.begin(), ins.end());
    } else if (at_line < 1) {
        lines.insert(lines.begin(), ins.begin(), ins.end());
    } else {
        lines.insert(lines.begin() + (at_line - 1), ins.begin(), ins.end());
    }
    return join_lines(lines);
}

// ================================================================
// Main node
// ================================================================

// ================================================================
// FileRdwtNode
// ================================================================
class FileRdwtNode : public rclcpp::Node {
public:
    FileRdwtNode(const std::string& agent_name)
        : Node("file_rdwt_node", agent_name), agent_name_(agent_name)
    {
        declare_parameter("agent_name", agent_name);
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", DEFAULT_TIMEOUT);

        default_timeout_ = get_parameter("default_timeout").as_double();

        // Action Server
        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this, "/" + agent_name_ + "/output/file_rdwt",
            [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
            [this](auto gh) { return handle_cancel(gh); },
            [this](auto gh) { handle_accepted(gh); });

        // Info 发布
        rclcpp::QoS qos(1);
        qos.transient_local(); qos.reliable();
        info_pub_ = create_publisher<std_msgs::msg::String>(
            "/" + agent_name_ + "/output/file_rdwt/info", qos);

        double info_rate = get_parameter("info_rate").as_double();
        publish_timer_ = create_wall_timer(
            std::chrono::duration<double>(1.0 / info_rate),
            [this]() { publish_info(); });
        publish_info();

        RCLCPP_INFO(get_logger(), "file_rdwt_node started");
    }

private:
    static constexpr double DEFAULT_TIMEOUT = 30.0;

    void publish_info() {
        std_msgs::msg::String msg;
        msg.data = FILE_RDWT_INFO_JSON;
        info_pub_->publish(msg);
    }

    rclcpp_action::CancelResponse handle_cancel(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        (void)goal_handle;
        RCLCPP_INFO(get_logger(), "handle_cancel: Cancel requested");
        canceled_.store(true);
        return rclcpp_action::CancelResponse::ACCEPT;
    }

    void handle_accepted(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        const auto goal = goal_handle->get_goal();

        // 1. 解析 JSON
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

        // 2. 校验 tool name
        if (!input.contains("name") || !input["name"].is_string() ||
            input["name"].get<std::string>() != "file_rdwt") {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Invalid or mismatched tool name");
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: bad tool name"})";
            goal_handle->abort(result);
            return;
        }

        // 3. 校验 arguments
        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing arguments");
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: missing arguments"})";
            goal_handle->abort(result);
            return;
        }

        auto& args = input["arguments"];

        // 4. 校验 action
        if (!args.contains("action") || !args["action"].is_string()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing action");
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: action is required"})";
            goal_handle->abort(result);
            return;
        }
        std::string action = args["action"].get<std::string>();
        if (action != "read" && action != "write" && action != "read_write") {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Unknown action: %s", action.c_str());
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: unknown action"})";
            goal_handle->abort(result);
            return;
        }

        // 5. 校验 path
        if (!args.contains("path") || !args["path"].is_string()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing path");
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: path is required"})";
            goal_handle->abort(result);
            return;
        }
        std::string path = args["path"].get<std::string>();
        if (path.empty() || path[0] != '/') {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Path must be absolute: %s", path.c_str());
            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = R"({"error":"invalid input: path must be absolute"})";
            goal_handle->abort(result);
            return;
        }

        // 启动执行线程
        canceled_.store(false);
        std::thread{[this, goal_handle]() { execute(goal_handle); }}.detach();
    }

    void execute(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        auto result = std::make_shared<ExecuteTool::Result>();
        const auto goal = goal_handle->get_goal();
        json input = json::parse(goal->input_json);
        auto& args = input["arguments"];

        // 提取参数
        std::string action  = args.value("action", "");
        std::string path    = args.value("path", "");
        std::string content = args.value("content", "");
        std::string mode    = args.value("mode", "overwrite");
        int64_t start_line  = -1;
        int64_t end_line    = -1;
        if (args.contains("range")) {
            auto& r = args["range"];
            if (r.contains("start_line")) start_line = r["start_line"].get<int64_t>();
            if (r.contains("end_line"))   end_line   = r["end_line"].get<int64_t>();
        }

        // 超时
        double timeout = default_timeout_;
        if (args.contains("timeout_sec") && args["timeout_sec"].is_number()) {
            double ts = args["timeout_sec"].get<double>();
            if (ts > 0.0) timeout = ts;
        }
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(timeout));

        // 辅助: 失败返回
        auto fail = [&](const std::string& msg) {
            RCLCPP_ERROR(get_logger(), "execute: %s", msg.c_str());
            result->output_json = make_error(msg).dump();
            goal_handle->abort(result);
        };

        // 二次校验（path 安全性等）
        if (path.find("..") != std::string::npos)
            return fail("path contains .. (rejected for safety)");
        if (fs::exists(path) && fs::is_directory(path))
            return fail("path is a directory");

        bool is_read  = (action == "read" || action == "read_write");
        bool is_write = (action == "write" || action == "read_write");

        if (is_write) {
            if (mode != "overwrite" && mode != "append" && mode != "insert")
                return fail("unknown mode: " + mode);
            if (content.empty())
                return fail("content is required");
        }
        if (start_line != -1 && start_line < 1)
            return fail("start_line must be >= 1");
        if (start_line != -1 && end_line != -1 && end_line < start_line)
            return fail("end_line < start_line");

        auto timed_out = [&]() { return std::chrono::steady_clock::now() > deadline; };

        // ---- Write ----
        int64_t written = 0;
        if (is_write) {
            if (mode == "insert" && (!args.contains("range") || !args["range"].contains("start_line")))
                return fail("insert mode requires range.start_line");

            std::string eff_mode = mode;
            if (mode == "insert" && fs::exists(path)) {
                // 退出原因: insert 需要先读原文件再写回
                std::ifstream ifs(path, std::ios::binary);
                if (!ifs.is_open())
                    return fail("cannot open for reading (insert prepare): " + path);
                std::string existing((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                ifs.close();
                content = content_insert(existing, content, (start_line > 0) ? start_line : 1);
                eff_mode = "overwrite";
            }

            std::ios::openmode om = std::ios::binary;
            if (eff_mode == "append" && fs::exists(path))
                om |= std::ios::app;
            else
                om |= std::ios::trunc;

            std::ofstream ofs(path, om);
            if (!ofs.is_open())
                return fail("cannot open for writing: " + path);

            // 分块写入，每块检查 cancel/timeout
            constexpr size_t CHUNK = 4096;
            size_t pos = 0;
            while (pos < content.size()) {
                // 退出原因: 写入期间被取消
                if (canceled_.load())
                    return fail("canceled by user");
                // 退出原因: 写入超时
                if (timed_out()) {
                    json err;
                    err["error"] = "timed out after " + std::to_string(timeout) + "s, "
                        + std::to_string(pos) + " bytes written";
                    return fail(err.dump());
                }
                size_t n = std::min(CHUNK, content.size() - pos);
                ofs.write(content.data() + pos, n);
                if (ofs.fail())
                    return fail("write failed at byte " + std::to_string(pos));
                pos += n;
            }
            ofs.close();
            written = static_cast<int64_t>(content.size());
        }

        // ---- Read ----
        std::string read_content;
        int64_t read_size = 0;
        if (is_read) {
            // 退出原因: 文件不存在
            if (!fs::exists(path))
                return fail("file not found: " + path);

            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.is_open())
                return fail("cannot open for reading: " + path);

            constexpr size_t READ_CHUNK = 4096;
            std::string raw;
            char buf[READ_CHUNK];
            while (ifs) {
                // 退出原因: 读取期间被取消
                if (canceled_.load())
                    return fail("canceled by user");
                // 退出原因: 读取超时
                if (timed_out())
                    return fail("timed out after " + std::to_string(timeout) + "s while reading");
                ifs.read(buf, READ_CHUNK);
                std::streamsize n = ifs.gcount();
                if (n > 0) raw.append(buf, n);
                else break;
            }
            ifs.close();

            // 退出原因: 读取完成后被取消
            if (canceled_.load())
                return fail("canceled by user");

            if (start_line > 0 || end_line > 0) {
                int64_t s = (start_line > 0) ? start_line : 1;
                int64_t e = (end_line > 0) ? end_line : -1;
                read_content = read_lines_range(raw, s, e);
            } else {
                read_content = raw;
            }
            read_size = static_cast<int64_t>(read_content.size());
        }

        // ---- 组装成功结果 ----
        // 正常退出: 读写操作成功完成
        json out;
        out["exit_code"] = 0;
        if (is_read) {
            out["content"] = read_content;
            out["size"] = read_size;
        }
        if (is_write) {
            out["written"] = written;
        }
        result->output_json = out.dump();
        RCLCPP_INFO(get_logger(), "execute: Completed, action=%s, path=%s, read=%ld, written=%ld",
                    action.c_str(), path.c_str(), read_size, written);
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

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto temp = std::make_shared<rclcpp::Node>("temp");
    temp->declare_parameter<std::string>("agent_name", "agent");
    std::string agent_name = temp->get_parameter("agent_name").as_string();
    temp.reset();
    auto node = std::make_shared<FileRdwtNode>(agent_name);
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
