// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// file_rdwt_node: unified file read/write tool for Cloud-Soul
// Action: /<agent>/output/file_rdwt (cs_interfaces::ExecuteTool)
// Info:   /<agent>/output/file_rdwt/info (std_msgs/String)

#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <map>
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
using GoalHandle = rclcpp_action::ServerGoalHandle<ExecuteTool>;
using json = nlohmann::json;

// ================================================================
// Utility: build error result
// ================================================================
static json make_error(const std::string& msg) {
    return {{"error", msg}};
}

// ================================================================
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
        declare_parameter("default_timeout", 60.0);

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
                auto it = active_.find(gh->get_goal_id());
                if (it != active_.end()) {
                    it->second->canceled.store(true);
                    RCLCPP_INFO(get_logger(), "Cancel requested");
                }
                return rclcpp_action::CancelResponse::ACCEPT;
            },
            // handle_accepted
            [this](const std::shared_ptr<GoalHandle> gh) {
                auto st = std::make_shared<ExecState>();
                st->canceled.store(false);
                active_[gh->get_goal_id()] = st;
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
        msg.data = R"json({
  "name": "file_rdwt",
  "description": "Read/write files with line range support (1-indexed). action:read|write|read_write. mode:overwrite|append|insert (insert uses range.start_line). Range out-of-bounds auto-clamps. exit_code=0 on success, -1 with error.",
  "parameters": {
    "type": "object",
    "properties": {
      "action": {"type":"string","enum":["read","write","read_write"]},
      "path": {"type":"string","description":"Absolute file path"},
      "content": {"type":"string","description":"Content for write/read_write"},
      "mode": {"type":"string","enum":["overwrite","append","insert"]},
      "range": {"type":"object","properties":{"start_line":{"type":"integer"},"end_line":{"type":"integer"}},"description":"1-indexed, insert uses start_line as insertion point"}
    },
    "required": ["action","path"]
  }
})json";
        info_pub_->publish(msg);
    }

    void execute(const std::shared_ptr<GoalHandle> gh,
                 std::shared_ptr<ExecState> st) {
        auto result = std::make_shared<ExecuteTool::Result>();
        const auto goal = gh->get_goal();

        // ---- Parse input JSON with nlohmann ----
        json args;
        try {
            args = json::parse(goal->input_json);
        } catch (const json::parse_error& e) {
            result->output_json = make_error(
                "invalid JSON: " + std::string(e.what())).dump();
            result->exit_code = -1;
            gh->abort(result);
            active_.erase(gh->get_goal_id());
            return;
        }

        // ---- Timeout ----
        double timeout = goal->timeout_sec > 0.0
            ? goal->timeout_sec : default_timeout_;
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
            active_.erase(gh->get_goal_id());
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

            std::string raw((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
            ifs.close();

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
        active_.erase(gh->get_goal_id());
    }

    std::string agent_name_;
    double default_timeout_ = 60.0;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr publish_timer_;
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
