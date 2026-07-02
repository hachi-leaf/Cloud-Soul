// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// ================================================================
// Cloud-Soul 技能按需加载节点
// ================================================================
//
// 作用:
//   扫描灵肤仓库 skills/ 目录下的技能文件（Markdown with YAML frontmatter），
//   支持 list（列出所有技能及加载条件）和 load（加载指定技能完整内容）。
//   线程化执行，支持取消和超时。
//
// 节点名: /<agent_name>/skills_loader_node
//
// 参数:
//   agent_name       (string, 必填)  Agent 命名空间
//   repo_dir         (string, 必填)  灵魂仓库根路径（skills 目录的父目录）
//   info_rate         (double, 1.0)  发布 Tools Info 的频率（Hz）
//   default_timeout   (double, 30.0)  默认超时秒数
//
// Action:
//   /<agent_name>/output/skills_loader  (ExecuteTool)
//     Goal: 接收 {"name":"skills_loader","arguments":{"action":"list|load","name":"..."}}
//     Result: output_json 为结构化 JSON
//     Cancel: 终止进行中的操作
//
// 上层传入 JSON 规范 (来自 output_mgmt):
//   {
//     "name": "skills_loader",
//     "arguments": {
//       "action": "list",                    // list=列出所有技能, load=加载指定技能
//       "name": "skill_name"                 // 仅 action=load 时需要
//     }
//   }
//
// 关键设计:
//   - 线程化执行，支持取消
//   - Info 仅在启动、list、load 三个时机发布（transient_local QoS 保证 LLM 可随时获取）
//   - Info 的 description 字段包含所有技能的完整 frontmatter，LLM 可据此判断加载条件
//   - YAML frontmatter 解析使用 std::regex（输入大小可控，安全）
//

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <thread>
#include <atomic>
#include <chrono>

using json = nlohmann::json;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ============================================================
// SkillsLoaderNode
// ============================================================
class SkillsLoaderNode : public rclcpp::Node {
public:
    explicit SkillsLoaderNode(const std::string& agent_name)
        : Node("skills_loader_node", agent_name), agent_name_(agent_name)
    {
        declare_parameter("agent_name", agent_name);
        declare_parameter("repo_dir", "");
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", 30.0);

        repo_dir_       = get_parameter("repo_dir").as_string();
        info_rate_      = get_parameter("info_rate").as_double();
        default_timeout_ = get_parameter("default_timeout").as_double();

        if (repo_dir_.empty()) {
            RCLCPP_FATAL(get_logger(), "repo_dir 参数不能为空");
            rclcpp::shutdown();
        }

        // Info 发布（transient_local，最后一条消息保留供 LLM 随时获取）
        std::string topic = "/" + agent_name_ + "/output/skills_loader/info";
        rclcpp::QoS qos(1);
        qos.transient_local();
        qos.reliable();
        info_pub_ = create_publisher<std_msgs::msg::String>(topic, qos);

        // 定时发布 info 作为心跳
        publish_timer_ = create_wall_timer(
            std::chrono::duration<double>(1.0 / info_rate_),
            [this]() { refresh_info(); });

        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this,
            "/" + agent_name_ + "/output/skills_loader",
            // handle_goal
            [](const rclcpp_action::GoalUUID&,
               std::shared_ptr<const ExecuteTool::Goal>) {
                return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
            },
            // handle_cancel
            [this](auto goal_handle) { return handle_cancel(goal_handle); },
            // handle_accepted
            [this](auto goal_handle) { handle_accepted(goal_handle); });

        RCLCPP_INFO(get_logger(),
            "SkillsLoaderNode ready. agent=%s repo_dir=%s",
            agent_name_.c_str(), repo_dir_.c_str());

        // 启动时发布一次 info
        refresh_info();
    }

    ~SkillsLoaderNode() override = default;

private:
    static constexpr const char* INFO_JSON = R"json({
  "type": "function",
  "function": {
    "name": "skills_loader",
    "description": "_placeholder_",
    "parameters": {
      "type": "object",
      "required": ["action"],
      "properties": {
        "action": {
          "type": "string",
          "enum": ["list", "load"],
          "description": "操作类型：list=列出所有可用技能的名称、描述和加载条件；load=加载指定技能的完整内容"
        },
        "name": {
          "type": "string",
          "description": "要加载的技能名称（仅 action=load 时需要）"
        }
      }
    }
  }
})json";

    // ---------------------------------------------------------
    // 字符串 trim
    // ---------------------------------------------------------
    static void trim(std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    }

    // ---------------------------------------------------------
    // 读取文件内容
    // ---------------------------------------------------------
    static std::string read_file(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return "";
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // ---------------------------------------------------------
    // YAML Frontmatter 解析
    // ---------------------------------------------------------
    static json parse_frontmatter(const std::string& filepath) {
        std::string content = read_file(filepath);
        json fm;

        std::regex fm_regex(R"(^---\s*\n([\s\S]*?)\n---)");
        std::smatch match;
        if (!std::regex_search(content, match, fm_regex)) return fm;

        std::string yaml_str = match[1].str();
        std::istringstream stream(yaml_str);
        std::string line, current_key, current_value;
        bool in_multiline = false;

        while (std::getline(stream, line)) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
                if (in_multiline) current_value += "\n";
                continue;
            }

            auto colon = line.find(':');
            if (colon != std::string::npos && !in_multiline) {
                if (!current_key.empty()) {
                    trim(current_value);
                    fm[current_key] = current_value;
                }
                current_key = line.substr(0, colon);
                trim(current_key);
                current_value = line.substr(colon + 1);
                trim(current_value);

                if (current_value.size() >= 2 &&
                    ((current_value.front() == '"' && current_value.back() == '"') ||
                     (current_value.front() == '\'' && current_value.back() == '\'')))
                    current_value = current_value.substr(1, current_value.size() - 2);

                if (current_value == "|") {
                    current_value = "";
                    in_multiline = true;
                }
            } else if (in_multiline) {
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

    // ---------------------------------------------------------
    // 发布 info
    // ---------------------------------------------------------
    void refresh_info() {
        auto msg = std_msgs::msg::String();
        msg.data = build_info_json();
        info_pub_->publish(msg);
    }

    // ---------------------------------------------------------
    // 构建 Info JSON（收集所有 skill 的完整 frontmatter）
    // ---------------------------------------------------------
    std::string build_info_json() {
        json info = json::parse(INFO_JSON);

        fs::path skills_dir = fs::path(repo_dir_) / "skills";
        std::string desc = "按需加载技能（Skill）。各技能详情：\n";

        if (fs::exists(skills_dir) && fs::is_directory(skills_dir)) {
            bool found = false;
            for (const auto& entry : fs::directory_iterator(skills_dir)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                if (ext != ".md" && ext != ".MD") continue;

                auto fm = parse_frontmatter(entry.path().string());
                if (!fm.contains("name")) continue;

                desc += "\n[" + entry.path().filename().string() + "]\n";
                for (auto& [key, val] : fm.items()) {
                    desc += key + ": " + val.get<std::string>() + "\n";
                }
                found = true;
            }
            if (!found) desc += "\n(无)\n";
        } else {
            desc += "\n(skills 目录不存在)\n";
        }
        desc += "\n用 load 加载完整内容，用 list 重新列出。";

        info["function"]["description"] = desc;
        return info.dump();
    }

    // ---------------------------------------------------------
    // handle_cancel — 设置 canceled_ 标志
    // ---------------------------------------------------------
    rclcpp_action::CancelResponse handle_cancel(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>>)
    {
        RCLCPP_INFO(get_logger(), "handle_cancel: Cancel requested");
        canceled_.store(true);
        return rclcpp_action::CancelResponse::ACCEPT;
    }

    // ---------------------------------------------------------
    // handle_accepted — 验证输入后在线程中执行
    // ---------------------------------------------------------
    void handle_accepted(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        const auto goal = goal_handle->get_goal();
        auto result = std::make_shared<ExecuteTool::Result>();

        json input;
        try {
            input = json::parse(goal->input_json);
        } catch (...) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Failed to parse input JSON");
            result->output_json = R"({"error":"invalid input JSON"})";
            result->exit_code = 1;
            goal_handle->abort(result);
            return;
        }

        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing arguments");
            result->output_json = R"({"error":"invalid input: missing arguments"})";
            result->exit_code = 1;
            goal_handle->abort(result);
            return;
        }

        auto& args = input["arguments"];
        if (!args.contains("action") || !args["action"].is_string()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing action");
            result->output_json = R"({"error":"invalid input: action is required"})";
            result->exit_code = 1;
            goal_handle->abort(result);
            return;
        }

        std::string action = args["action"].get<std::string>();
        std::string name = args.value("name", "");

        canceled_.store(false);
        work_thread_ = std::thread([goal_handle, action, name, this]() {
            json response;
            try {
                if (action == "list") {
                    response = action_list();
                } else if (action == "load") {
                    if (name.empty())
                        response = {{"error", "name is required for action=load"}};
                    else
                        response = action_load(name);
                } else {
                    response = {{"error", "unknown action: " + action}};
                }
            } catch (const std::exception& e) {
                response = {{"error", std::string("exception: ") + e.what()}};
            }

            if (canceled_.load()) {
                RCLCPP_INFO(get_logger(), "execute: Canceled before result");
                auto aborted = std::make_shared<ExecuteTool::Result>();
                goal_handle->abort(aborted);
                return;
            }

            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = response.dump();
            result->exit_code = response.contains("error") ? 1 : 0;
            goal_handle->succeed(result);

            // 执行完后刷新 info
            refresh_info();
        });
        work_thread_.detach();
    }

    // ---------------------------------------------------------
    // action=list — 列出所有技能
    // ---------------------------------------------------------
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
            // 包含完整的 frontmatter 字段（如 trigger）
            for (auto& [key, val] : fm.items()) {
                if (key != "name" && key != "description" && key != "file")
                    s[key] = val;
            }
            skills.push_back(s);
        }
        return {{"skills", skills}, {"count", skills.size()}};
    }

    // ---------------------------------------------------------
    // action=load — 加载指定技能
    // ---------------------------------------------------------
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
            json resp = {
                {"name", name},
                {"content", c},
                {"description", fm.value("description", "")},
                {"file", entry.path().filename().string()}
            };
            // 附加其他 frontmatter 字段
            for (auto& [key, val] : fm.items()) {
                if (key != "name" && key != "description" && key != "file")
                    resp[key] = val;
            }
            return resp;
        }
        return {{"error", "skill not found: " + name}};
    }

    // 成员变量
    std::string agent_name_;
    std::string repo_dir_;
    double info_rate_;
    double default_timeout_;
    std::atomic<bool> canceled_{false};
    std::thread work_thread_;

    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr publish_timer_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
};

// ============================================================
// main
// ============================================================
int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto temp = std::make_shared<rclcpp::Node>("temp");
    temp->declare_parameter<std::string>("agent_name", "agent");
    std::string agent_name = temp->get_parameter("agent_name").as_string();
    temp.reset();
    auto node = std::make_shared<SkillsLoaderNode>(agent_name);
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
