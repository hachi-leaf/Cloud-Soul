// Copyright (c) leaf
// SPDX-License-Identifier: MIT
//
// skills_loader_node — 按需加载 Skill

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <regex>

using json = nlohmann::json;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;
namespace fs = std::filesystem;

class SkillsLoaderNode : public rclcpp::Node {
public:
    SkillsLoaderNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions())
        : Node("skills_loader_node", options) {

        declare_parameter("agent_name", "");
        declare_parameter("repo_dir", "");
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", 30.0);

        agent_name_     = get_parameter("agent_name").as_string();
        repo_dir_       = get_parameter("repo_dir").as_string();
        info_rate_      = get_parameter("info_rate").as_double();
        default_timeout_ = get_parameter("default_timeout").as_double();

        if (agent_name_.empty()) {
            RCLCPP_FATAL(get_logger(), "agent_name 参数不能为空");
            rclcpp::shutdown(nullptr, "agent_name 为空");
        }
        if (repo_dir_.empty()) {
            RCLCPP_FATAL(get_logger(), "repo_dir 参数不能为空");
            rclcpp::shutdown(nullptr, "repo_dir 为空");
        }

        std::string topic = "/" + agent_name_ + "/output/skills_loader/info";
        rclcpp::QoS qos(1);
        qos.transient_local();
        qos.reliable();
        info_pub_ = create_publisher<std_msgs::msg::String>(topic, qos);
        info_timer_ = create_wall_timer(
            std::chrono::milliseconds(static_cast<int>(1000.0 / info_rate_)),
            [this]() {
                auto msg = std_msgs::msg::String();
                msg.data = build_info_json();
                info_pub_->publish(msg);
            });

        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this, "/" + agent_name_ + "/output/skills_loader",
            [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
            [](auto...) { return rclcpp_action::CancelResponse::ACCEPT; },
            [this](auto goal_handle) { handle_accepted(goal_handle); });

        RCLCPP_INFO(get_logger(),
            "skills_loader 启动: agent=%s, repo_dir=%s", agent_name_.c_str(), repo_dir_.c_str());
        refresh_info();
    }

private:
    // ─── 字符串 trim ─────────────────────────────────────────
    static void trim(std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    }

    // ─── 刷新 info 话题 ──────────────────────────────────────
    void refresh_info() {
        auto msg = std_msgs::msg::String();
        msg.data = build_info_json();
        info_pub_->publish(msg);
    }

    // ─── 构建动态 Info JSON ──────────────────────────────────
    std::string build_info_json() {
        json info = json::parse(R"({
            "type": "function",
            "function": {
                "name": "skills_loader",
                "parameters": {
                    "type": "object",
                    "required": ["action"],
                    "properties": {
                        "action": {
                            "type": "string",
                            "enum": ["list", "load"],
                            "description": "操作类型：list=列出所有可用技能的名称和描述；load=加载指定技能的完整内容"
                        },
                        "name": {
                            "type": "string",
                            "description": "要加载的技能名称（仅 action=load 时需要）"
                        }
                    }
                }
            }
        })");

        fs::path skills_dir = fs::path(repo_dir_) / "skills";
        std::string desc = "按需加载技能（Skill）。可用技能：\n";
        if (fs::exists(skills_dir) && fs::is_directory(skills_dir)) {
            bool found = false;
            for (const auto& entry : fs::directory_iterator(skills_dir)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                if (ext != ".md" && ext != ".MD") continue;
                auto fm = parse_frontmatter(entry.path().string());
                if (!fm.contains("name")) continue;
                desc += "- " + fm["name"].get<std::string>();
                if (fm.contains("description") && !fm["description"].get<std::string>().empty()) {
                    desc += ": " + fm["description"].get<std::string>();
                }
                desc += "\n";
                found = true;
            }
            if (!found) desc += "(无)\n";
        } else {
            desc += "(skills 目录不存在)\n";
        }
        desc += "用 load 加载完整内容，用 list 重新列出。";
        info["function"]["description"] = desc;
        return info.dump();
    }

    // ─── 处理 Goal ────────────────────────────────────────────
    void handle_accepted(std::shared_ptr<GoalHandleExecute> goal_handle) {
        auto goal = goal_handle->get_goal();
        json input;
        try { input = json::parse(goal->input_json); } catch (...) {
            auto r = std::make_shared<ExecuteTool::Result>();
            r->output_json = R"({"error":"invalid JSON"})"; r->exit_code = -1;
            goal_handle->abort(r); return;
        }
        if (!input.contains("name") || !input["name"].is_string() || input["name"] != "skills_loader") {
            auto r = std::make_shared<ExecuteTool::Result>();
            r->output_json = R"({"error":"invalid input: mismatched tool name"})"; r->exit_code = -1;
            goal_handle->abort(r); return;
        }
        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            auto r = std::make_shared<ExecuteTool::Result>();
            r->output_json = R"({"error":"invalid input: missing arguments"})"; r->exit_code = -1;
            goal_handle->abort(r); return;
        }
        if (!input["arguments"].contains("action") || !input["arguments"]["action"].is_string()) {
            auto r = std::make_shared<ExecuteTool::Result>();
            r->output_json = R"({"error":"invalid input: action is required"})"; r->exit_code = -1;
            goal_handle->abort(r); return;
        }

        std::string action = input["arguments"]["action"].get<std::string>();
        std::string name = input["arguments"].value("name", "");

        json response;
        try {
            if (action == "list") response = action_list();
            else if (action == "load") {
                if (name.empty()) response = {{"error", "name is required for action=load"}};
                else response = action_load(name);
            }
            else response = {{"error", "unknown action: " + action}};
        } catch (const std::exception& e) {
            response = {{"error", std::string("exception: ") + e.what()}};
        }

        auto r = std::make_shared<ExecuteTool::Result>();
        r->output_json = response.dump();
        r->exit_code = response.contains("error") ? -1 : 0;
        goal_handle->succeed(r);
        refresh_info();
    }

    // ─── action=list ──────────────────────────────────────────
    json action_list() {
        fs::path skills_dir = fs::path(repo_dir_) / "skills";
        if (!fs::exists(skills_dir) || !fs::is_directory(skills_dir))
            return {{"error", "skills directory not found: " + skills_dir.string()}};

        json skills = json::array();
        for (const auto& entry : fs::directory_iterator(skills_dir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".md" && ext != ".MD") continue;
            auto fm = parse_frontmatter(entry.path().string());
            if (!fm.contains("name")) continue;
            json s;
            s["name"] = fm["name"];
            s["description"] = fm.value("description", "");
            s["file"] = entry.path().filename().string();
            skills.push_back(s);
        }
        return {{"skills", skills}, {"count", skills.size()}};
    }

    // ─── action=load ──────────────────────────────────────────
    json action_load(const std::string& name) {
        fs::path skills_dir = fs::path(repo_dir_) / "skills";
        if (!fs::exists(skills_dir) || !fs::is_directory(skills_dir))
            return {{"error", "skills directory not found"}};

        for (const auto& entry : fs::directory_iterator(skills_dir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".md" && ext != ".MD") continue;
            auto fm = parse_frontmatter(entry.path().string());
            if (!fm.contains("name") || fm["name"] != name) continue;
            std::string c = read_file(entry.path().string());
            return {{"name", name}, {"content", c}, {"description", fm.value("description", "")}, {"file", entry.path().filename().string()}};
        }
        return {{"error", "skill not found: " + name}};
    }

    // ─── YAML Frontmatter 解析 ────────────────────────────────
    json parse_frontmatter(const std::string& filepath) {
        std::string content = read_file(filepath);
        json fm;
        std::regex fm_regex(R"(^---\s*\n([\s\S]*?)\n---)");
        std::smatch match;
        if (!std::regex_search(content, match, fm_regex)) return fm;

        // 逐行解析 YAML，支持多行值（| 符号）
        std::string yaml_str = match[1].str();
        std::istringstream stream(yaml_str);
        std::string line, current_key, current_value;
        bool in_multiline = false;

        while (std::getline(stream, line)) {
            // 跳过空行
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
                if (in_multiline) current_value += "\n";
                continue;
            }

            auto colon = line.find(':');
            if (colon != std::string::npos && !in_multiline) {
                // 新 key: value
                if (!current_key.empty()) {
                    trim(current_value);
                    fm[current_key] = current_value;
                }
                current_key = line.substr(0, colon);
                trim(current_key);
                current_value = line.substr(colon + 1);
                trim(current_value);

                // 去掉引号
                if (current_value.size() >= 2 &&
                    ((current_value.front() == '"' && current_value.back() == '"') ||
                     (current_value.front() == '\'' && current_value.back() == '\'')))
                    current_value = current_value.substr(1, current_value.size() - 2);

                // YAML 多行值 |
                if (current_value == "|") {
                    current_value = "";
                    in_multiline = true;
                }
            } else if (in_multiline) {
                // 多行值的续行
                std::string trimmed = line;
                trim(trimmed);
                if (!current_value.empty()) current_value += " ";
                current_value += trimmed;
            }
        }
        if (!current_key.empty()) {
            trim(current_value);
            fm[current_key] = current_value;
        }
        return fm;
    }

    std::string read_file(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return "";
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    std::string agent_name_;
    std::string repo_dir_;
    double info_rate_;
    double default_timeout_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr info_timer_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<SkillsLoaderNode>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
