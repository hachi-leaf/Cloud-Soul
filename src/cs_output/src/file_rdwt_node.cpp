// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul 标准 output 工具节点：文件读写 
// unified file read/write tool for Cloud-Soul

// Node: /<agent_name>/file_rdwt_node
// Param:
//  <string>agent_name       --> Agent 名
//  <float64>info_rate       --> 发布 Tools Info 的频率（Hz）
//  <float64>default_timeout --> 文件操作超时（秒），当 LLM 未传 timeout_sec 时使用，默认 30.0

// Topic: /<agent_name>/output/file_rdwt/info
// Struct:
//  <string>info --> 输入给 LLM 的 tools json 字段，见 FILE_RDWT_INFO_JSON

// Action: /<agent_name>/output/file_rdwt
// Struct:
//  Goal <string>input_json      --> LLM 输出的 tools_call json 字段，由 FILE_RDWT_INFO_JSON 约束
//  ---
//  Results <string>output_json  --> 返回给 LLM tools_callback 字段，为自由字符串
//  Results <int32>exit_code     --> 错误码，0 为成功，其他值为 Error
//  ---
//  Feedback <string>status      --> reserved

// Action 特性：
//  1. 禁止并行，并行时直接对新 Goal 返回 exit_code = -1, output_json = {"error":"Another goal is already running"}
//  2. 读超时: exit_code = -1, output_json = {"error":"timed out after Xs while reading"}（X 为 timeout 秒数）
//  3. 写超时: exit_code = -1, output_json = {"error":"timed out after Xs, Y bytes written"}（X 同超时，Y 为已写入字节数）
//  4. 写后读超时: 若超时发生在写阶段同 3，发生在读阶段同 2，整体共享同一个超时
//  5. 所有输入校验失败 (action/path/mode/content/range) 均返回 exit_code = -1, output_json = {"error":"..."}，错误原因包括：
//     - "unknown action: …"
//     - "path is required" / "path must be absolute" / "path contains .. (rejected for safety)" / "path is a directory"
//     - "unknown mode: …"
//     - "content is required"
//     - "start_line must be >= 1" / "end_line < start_line"
//     - "insert mode requires range.start_line"
//  6. 文件不存在时：读返回 {"error":"file not found"}；写自动创建后正常写入
//  7. 系统错误（无权限、磁盘满等）返回对应错误，exit_code = -1，例如：
//     - "cannot open for writing: …" / "cannot open for reading: …" / "cannot open for reading (insert prepare): …"
//     - "write failed at byte N"
//  8. 用户主动 Cancel 返回 exit_code = -1, output_json = {"error":"canceled by user"}
//  9. 输入 JSON 格式错误时自动尝试修复（去尾逗号、补全括号等），修复仍失败则返回 exit_code = -1, output_json = {"error":"invalid JSON: …"}
// 10. 成功返回 exit_code = 0：
//     - 读：output_json = {"exit_code":0,"content":"...","size":N}
//     - 写：output_json = {"exit_code":0,"written":N}
//     - 读写：output_json = {"exit_code":0,"content":"...","size":N,"written":N}
// 11. range 越界自动 clamp（不报错）；insert 超出文件行数时退化为追加（等效 append）

#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <map>
#include <vector>
#include <filesystem>
#include <mutex>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "nlohmann/json.hpp"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandle = rclcpp_action::ServerGoalHandle<ExecuteTool>;
using json = nlohmann::json;

// ================================================================
// Tool Description (DeepSeek/OpenAI function-calling compatible)
// ================================================================
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
        }
      }
    }
  }
})json";


// ================================================================
// Utility: build error result
// ================================================================
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
class FileRdwtNode : public rclcpp::Node {
public:
    explicit FileRdwtNode(const std::string& agent_name)
        : Node("file_rdwt_node", agent_name), agent_name_(agent_name)
    {
        declare_parameter("agent_name", agent_name);
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", 30.0);

        double info_rate = get_parameter("info_rate").as_double();
        default_timeout_ = get_parameter("default_timeout").as_double();

        std::string ns = "/" + agent_name_;

        // ---- Action Server ----
        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this, ns + "/output/file_rdwt",
            // handle_goal
            [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
            // handle_cancel
            [this](const std::shared_ptr<GoalHandle> gh) {
                std::lock_guard<std::mutex> lock(active_mutex_);
                auto it = active_.find(gh->get_goal_id());
                if (it != active_.end()) {
                    it->second->canceled.store(true);
                    RCLCPP_INFO(get_logger(), "Cancel requested");
                }
                return rclcpp_action::CancelResponse::ACCEPT;
            },
            // handle_accepted
            [this](const std::shared_ptr<GoalHandle> gh) {
                std::shared_ptr<ExecState> st;
                {
                    std::lock_guard<std::mutex> lock(active_mutex_);
                    if (!active_.empty()) {
                        auto result = std::make_shared<ExecuteTool::Result>();
                        result->output_json = make_error("Another goal is already running").dump();
                        result->exit_code = -1;
                        gh->abort(result);
                        RCLCPP_WARN(get_logger(), "Rejected new goal: another goal is active");
                        return;
                    }
                    st = std::make_shared<ExecState>();
                    st->canceled.store(false);
                    active_[gh->get_goal_id()] = st;
                }
                std::thread{std::bind(&FileRdwtNode::execute, this, gh, st)}.detach();
            }
        );

        // ---- Info publisher ----
        rclcpp::QoS qos(1);
        qos.transient_local(); qos.reliable();
        info_pub_ = create_publisher<std_msgs::msg::String>(
            ns + "/output/file_rdwt/info", qos);

        publish_timer_ = create_wall_timer(
            std::chrono::duration<double>(1.0 / info_rate),
            [this]{ publish_info(); });
        publish_info();

        RCLCPP_INFO(get_logger(), "file_rdwt_node started for %s", agent_name_.c_str());
    }

private:
    struct ExecState { std::atomic<bool> canceled{false}; };

    void publish_info() {
        std_msgs::msg::String msg;
        msg.data = FILE_RDWT_INFO_JSON;
        info_pub_->publish(msg);
    }

    void execute(const std::shared_ptr<GoalHandle> gh,
                 std::shared_ptr<ExecState> st) {
        auto result = std::make_shared<ExecuteTool::Result>();
        const auto goal = gh->get_goal();

        // ---- Parse input JSON (defensive: auto-repair on failure) ----
        json args;
        try {
            args = json::parse(goal->input_json);
        } catch (const json::parse_error& e) {
            std::string fixed = repair_json(goal->input_json);
            try {
                args = json::parse(fixed);
                RCLCPP_INFO(get_logger(), "JSON auto-repaired");
            } catch (const json::parse_error& e2) {
                result->output_json = make_error(
                    "invalid JSON: " + std::string(e.what())).dump();
                result->exit_code = -1;
                gh->abort(result);
                {
                    std::lock_guard<std::mutex> lock(active_mutex_);
                    active_.erase(gh->get_goal_id());
                }
                return;
            }
        }

        // ---- Timeout ----
        double timeout = default_timeout_;
      try {
        json input = json::parse(goal->input_json);
        if (input.contains("timeout_sec") && input["timeout_sec"].is_number()) {
          double ts = input["timeout_sec"].get<double>();
          if (ts > 0.0) timeout = ts;
        }
      } catch (...) {}
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::duration<double>(timeout);

        // ---- Extract fields ----
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

        // ---- Validation ----
        auto fail = [&](const std::string& msg) {
            result->output_json = make_error(msg).dump();
            result->exit_code = -1;
            gh->abort(result);
            {
                std::lock_guard<std::mutex> lock(active_mutex_);
                active_.erase(gh->get_goal_id());
            }
        };

        if (path.empty())                    return fail("path is required");
        if (path[0] != '/')                  return fail("path must be absolute");
        if (path.find("..") != std::string::npos)
                                             return fail("path contains .. (rejected for safety)");
        if (fs::exists(path) && fs::is_directory(path))
                                             return fail("path is a directory");
        if (action != "read" && action != "write" && action != "read_write")
            return fail("unknown action: " + action + ", expected read|write|read_write");

        bool is_read  = (action == "read" || action == "read_write");
        bool is_write = (action == "write" || action == "read_write");

        if (is_write) {
            if (mode != "overwrite" && mode != "append" && mode != "insert")

                return fail("unknown mode: " + mode + ", expected overwrite|append|insert");
            if (content.empty())
                return fail("content is required");
        }

        if (start_line != -1 && start_line < 1)
            return fail("start_line must be >= 1");
        if (start_line != -1 && end_line != -1 && end_line < start_line)
            return fail("end_line < start_line");

        // ---- Timeout check helper ----
        auto timed_out = [&]() -> bool {
            return std::chrono::steady_clock::now() > deadline;
        };

        // ---- Write ----
        int64_t written = 0;
        if (is_write) {
            // compute effective mode for insert with start_line
            if (mode == "insert" && (!args.contains("range") || !args["range"].contains("start_line")))
                return fail("insert mode requires range.start_line");
            std::string eff_mode = mode;

            // For insert: read existing content, modify, write back
            if (mode == "insert" && fs::exists(path)) {
                std::ifstream ifs(path, std::ios::binary);
                if (!ifs.is_open())
                    return fail("cannot open for reading (insert prepare): " + path);
                std::string existing((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                ifs.close();
                int64_t target_line = (start_line > 0) ? start_line : 1;
                std::string combined = content_insert(existing, content, target_line);
                content = combined;
                eff_mode = "overwrite";  // write the combined result
            }

            std::ios::openmode om = std::ios::binary;

            // insert mode requires range.start_line
            if (eff_mode == "append" && fs::exists(path))
                om |= std::ios::app;
            else
                om |= std::ios::trunc;

            std::ofstream ofs(path, om);
            if (!ofs.is_open())
                return fail("cannot open for writing: " + path);

            // chunked write with cancel/timeout checks
            constexpr size_t CHUNK = 4096;
            size_t pos = 0;
            while (pos < content.size()) {
                if (st->canceled.load())
                    return fail("canceled by user");
                if (timed_out())
                    return fail("timed out after " + std::to_string(timeout)
                        + "s, " + std::to_string(pos) + " bytes written");
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
            if (!fs::exists(path))
                return fail("file not found: " + path);

            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.is_open())
                return fail("cannot open for reading: " + path);

            constexpr size_t READ_CHUNK = 4096;
            std::string raw;
            raw.reserve(4096);          // 可调，避免频繁重分配
            char buf[READ_CHUNK];
            while (ifs) {
                if (st->canceled.load())
                    return fail("canceled by user");
                if (timed_out())
                    return fail("timed out after " + std::to_string(timeout) + "s while reading");
                ifs.read(buf, READ_CHUNK);
                std::streamsize n = ifs.gcount();
                if (n > 0)
                    raw.append(buf, n);
                else
                    break;  // EOF or error
            }
            ifs.close();

            // 读取完成后再取消也要报（与写入逻辑对称）
            if (st->canceled.load())
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

        // ---- Build result ----
        if (st->canceled.load()) {
            result->output_json = make_error("canceled by user").dump();
            result->exit_code = -1;
        } else {
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
            result->exit_code = 0;
        }

        gh->succeed(result);
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_.erase(gh->get_goal_id());
        }
    }

    std::string agent_name_;
    double default_timeout_ = 60.0;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr publish_timer_;
    std::mutex active_mutex_;
    std::map<rclcpp_action::GoalUUID, std::shared_ptr<ExecState>> active_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto temp = std::make_shared<rclcpp::Node>("temp");
    temp->declare_parameter("agent_name", "agent");
    std::string an = temp->get_parameter("agent_name").as_string();
    temp.reset();
    auto node = std::make_shared<FileRdwtNode>(an);
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}