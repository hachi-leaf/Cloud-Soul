// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul Agent 主循环节点 - 扁平化极简版
// Node: /<agent_name>/agent_loop_node

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <regex>

#include "cs_core/call_openai.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include "cs_interfaces/srv/get_snapshot.hpp"
#include "cs_interfaces/srv/get_tools_info.hpp"
#include "cs_interfaces/srv/memory_archive.hpp"
#include "cs_interfaces/srv/memory_recall.hpp"
#include "nlohmann/json.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"

using namespace std::chrono_literals;
using json = nlohmann::json;

using MemoryRecall = cs_interfaces::srv::MemoryRecall;
using MemoryArchive = cs_interfaces::srv::MemoryArchive;
using GetSnapshot = cs_interfaces::srv::GetSnapshot;
using GetToolsInfo = cs_interfaces::srv::GetToolsInfo;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ClientGoalHandle<ExecuteTool>;

class AgentLoopNode : public rclcpp::Node {
public:
    AgentLoopNode(const std::string& agent_name)
        : Node("agent_loop_node", agent_name), agent_name_(agent_name)
    {
        declare_parameter<std::string>("agent_name", agent_name);
        declare_parameter<std::string>("context_dir", "");
        declare_parameter<int>("max_context_tokens", 32768);
        declare_parameter<int>("summary_turns", 10);
        declare_parameter<std::string>("openai_base_url", "");
        declare_parameter<std::string>("openai_api_key", "");
        declare_parameter<std::string>("openai_model", "");

        context_dir_ = get_parameter("context_dir").as_string();
        max_context_tokens_ = get_parameter("max_context_tokens").as_int();
        summary_turns_ = get_parameter("summary_turns").as_int();
        std::string openai_base_url = get_parameter("openai_base_url").as_string();
        std::string openai_api_key = get_parameter("openai_api_key").as_string();
        std::string openai_model = get_parameter("openai_model").as_string();

        if (context_dir_.empty()) {
            throw std::runtime_error("context_dir 参数不能为空");
        }

        openai_client_ = std::make_unique<openai_client::OpenAIClient>(
            openai_base_url, openai_api_key, openai_model);

        memory_recall_client_ = create_client<MemoryRecall>(
            "/" + agent_name_ + "/memory_recall");
        memory_archive_client_ = create_client<MemoryArchive>(
            "/" + agent_name_ + "/memory_archive");
        snapshot_client_ = create_client<GetSnapshot>(
            "/" + agent_name_ + "/input");
        tools_info_client_ = create_client<GetToolsInfo>(
            "/" + agent_name_ + "/output/info");

        output_action_client_ = rclcpp_action::create_client<ExecuteTool>(
            this, "/" + agent_name_ + "/output");
    }

    void run() {
        init_message_list();

        while (rclcpp::ok()) {
            RCLCPP_INFO(get_logger(), "进入大循环，当前消息数: %zu", msg_history_.size());

            bool need_compress = false;
            while (rclcpp::ok()) {
                need_compress = small_loop_once();
                save_current_json();

                RCLCPP_INFO(get_logger(), "当前 token: %d, 阈值: %d",
                            current_input_tokens_, max_context_tokens_);
                if (need_compress) {
                    RCLCPP_INFO(get_logger(), "输入 token 超限 (%d > %d), 准备压缩",
                                current_input_tokens_, max_context_tokens_);
                    break;
                }

                if (!rclcpp::ok()) {
                    RCLCPP_INFO(get_logger(), "收到退出信号，退出小循环");
                    break;
                }
            }

            if (!rclcpp::ok()) break;

            if (need_compress) {
                bool compress_ok = false;
                std::string summary;
                json recent_msgs = json::array();
                int start_idx = std::max(0, (int)msg_history_.size() - summary_turns_);
                for (int i = start_idx; i < (int)msg_history_.size(); ++i) {
                    recent_msgs.push_back(msg_history_[i]);
                }


                // 去掉 recent_msgs 开头连续的 tool 消息，避免 LLM 报错
                // （tool 消息必须以 assistant(tool_calls) 开头，否则违反 API 规范）
                while (!recent_msgs.empty() && recent_msgs[0].value("role", "") == "tool") {
                    recent_msgs.erase(recent_msgs.begin());
                }
                while (rclcpp::ok()) {
                    save_current_json();
                    auto archive_req = std::make_shared<MemoryArchive::Request>();
                    archive_req->json_path = current_json_path_;
                    auto future = memory_archive_client_->async_send_request(archive_req);
                    if (!wait_for_future(future, std::chrono::seconds(180))) {
                        if (!rclcpp::ok()) break;
                        RCLCPP_WARN(get_logger(), "归档服务超时，重试...");
                        continue;
                    }
                    auto res = future.get();
                    if (res->error_code == 0) {
                        summary = res->message;
                        compress_ok = true;
                        break;
                    } else {
                        RCLCPP_WARN(get_logger(), "归档失败 (error=%d): %s, 重试...",
                                    res->error_code, res->message.c_str());
                    }
                    if (!rclcpp::ok()) break;
                }
                if (!compress_ok || !rclcpp::ok()) break;

                RCLCPP_INFO(get_logger(), "归档成功，概要长度: %zu", summary.size());

                json new_msgs = json::array();
                std::string sys_prompt;
                if (!fetch_system_prompt(sys_prompt)) {
                    RCLCPP_ERROR(get_logger(), "无法获取系统提示词，退出");
                    break;
                }
                new_msgs.push_back({{"role", "system"}, {"content", sys_prompt}});
                std::string summary_content = "记忆压缩前的内容概要：" + summary;
                new_msgs.push_back({{"role", "user"}, {"content", summary_content}});
                for (const auto& msg : recent_msgs) {
                    new_msgs.push_back(msg);
                }
                msg_history_ = new_msgs;
                create_new_json_file();
                save_current_json();

                // 压缩后不立即调用 LLM，token 计为 0，让主循环自然处理
                current_input_tokens_ = 0;
                needs_post_compress_macro_ = true;  // 下次快照注入宏文本
                RCLCPP_INFO(get_logger(), "压缩完成，新消息数: %zu", msg_history_.size());
                continue;
            } else {
                break;
            }
        }
        RCLCPP_INFO(get_logger(), "Agent 主循环结束");
    }

private:
    // ---------- 初始化与持久化 ----------
    void init_message_list() {
        std::string latest_file = find_latest_json();
        if (!latest_file.empty()) {
            RCLCPP_INFO(get_logger(), "加载已有对话: %s", latest_file.c_str());
            load_json_file(latest_file);
            current_json_path_ = latest_file;
        } else {
            RCLCPP_INFO(get_logger(), "无已有对话，创建新对话");
            std::string sys_prompt;
            if (!fetch_system_prompt(sys_prompt)) {
                RCLCPP_ERROR(get_logger(), "无法获取系统提示词，使用默认提示词");
                sys_prompt = "你是 Adam，一个 AI Agent。请使用工具与用户交互。";
            }
            msg_history_ = json::array();
            msg_history_.push_back({{"role", "system"}, {"content", sys_prompt}});
            msg_history_.push_back({{"role", "user"}, {"content", "新对话开始"}});
            msg_history_.push_back({{"role", "assistant"}, {"content", "新对话已启动，等待指令。"}});
            create_new_json_file();
            save_current_json();
        }
    }

    std::string find_latest_json() {
        DIR* dir = opendir(context_dir_.c_str());
        if (!dir) {
            mkdir(context_dir_.c_str(), 0755);
            return "";
        }
        std::string latest;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() < 5 || name.compare(name.size()-5, 5, ".json") != 0) continue;
            if (name.find(agent_name_) != std::string::npos) {
                if (latest.empty() || name > latest) {
                    latest = name;
                }
            }
        }
        closedir(dir);
        if (latest.empty()) return "";
        return context_dir_ + "/" + latest;
    }

    void create_new_json_file() {
        std::time_t now = std::time(nullptr);
        std::tm* gmt = std::gmtime(&now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "UTC-%Y-%m-%d-%H-%M-%S-", gmt);
        std::string filename = std::string(buf) + agent_name_ + ".json";
        current_json_path_ = context_dir_ + "/" + filename;
    }

    void save_current_json() {
        std::ofstream ofs(current_json_path_);
        if (!ofs) {
            RCLCPP_ERROR(get_logger(), "无法写入文件: %s", current_json_path_.c_str());
            return;
        }
        for (const auto& msg : msg_history_) {
            ofs << msg.dump() << "\n";
        }
        ofs.close();
    }

    void load_json_file(const std::string& path) {
        msg_history_ = json::array();
        std::ifstream ifs(path);
        if (!ifs) {
            RCLCPP_ERROR(get_logger(), "无法打开文件: %s", path.c_str());
            return;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ifs.close();

        try {
            json j = json::parse(content);
            if (j.is_array()) {
                msg_history_ = j;
                RCLCPP_INFO(get_logger(), "以 JSON 数组格式加载 %zu 条消息", msg_history_.size());
                return;
            }
        } catch (...) {}

        std::istringstream iss(content);
        std::string line;
        int line_num = 0;
        while (std::getline(iss, line)) {
            line_num++;
            if (line.empty()) continue;
            if (line_num == 1 && line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                line = line.substr(3);
                if (line.empty()) continue;
            }
            try {
                msg_history_.push_back(json::parse(line));
            } catch (const json::parse_error& e) {
                RCLCPP_WARN(get_logger(), "行 %d JSON 解析错误: %s", line_num, e.what());
            }
        }
        RCLCPP_INFO(get_logger(), "逐行解析完成，共加载 %zu 条消息", msg_history_.size());
    }

    bool fetch_system_prompt(std::string& prompt) {
        if (!memory_recall_client_->wait_for_service(std::chrono::seconds(5))) {
            RCLCPP_ERROR(get_logger(), "memory_recall 服务不可用");
            return false;
        }
        auto req = std::make_shared<MemoryRecall::Request>();
        auto future = memory_recall_client_->async_send_request(req);
        if (!wait_for_future(future, std::chrono::seconds(10))) {
            RCLCPP_ERROR(get_logger(), "memory_recall 服务超时");
            return false;
        }
        auto res = future.get();
        if (res->error_code == 0) {
            prompt = res->text;
            return true;
        }
        RCLCPP_ERROR(get_logger(), "memory_recall 返回错误: %d", res->error_code);
        return false;
    }

    // ---------- 小循环调度 ----------
    bool small_loop_once() {
        if (msg_history_.empty()) {
            RCLCPP_FATAL(get_logger(), "消息列表为空！");
            return false;
        }

        json& last_msg = msg_history_.back();

        if (!check_msg(last_msg)) {
            // 消息无效，弹出无效消息，注入提醒
            msg_history_.erase(msg_history_.size() - 1);
            msg_history_.push_back({
                {"role", "user"},
                {"content", "[系统] 检测到无效的消息格式，已忽略。请继续。"}
            });
            save_current_json();
            return false;
        }

        std::string role = last_msg["role"].get<std::string>();

        if (role == "system")    return response_system();
        if (role == "user")      return response_user();
        if (role == "assistant") return response_assistant();
        if (role == "tool")      return response_tool();

        RCLCPP_WARN(get_logger(), "未知角色: %s", role.c_str());
        return false;
    }

    // ---------- 消息校验 ----------
    bool check_msg(json& msg) {
        // 1. 检查 role 字段
        if (!msg.contains("role") || !msg["role"].is_string()) {
            RCLCPP_WARN(get_logger(), "消息缺少有效 role 字段");
            return false;
        }
        std::string role = msg["role"].get<std::string>();
        if (role != "system" && role != "user" && role != "assistant" && role != "tool") {
            RCLCPP_WARN(get_logger(), "未知 role: %s", role.c_str());
            return false;
        }

        // 2. 检查 content 字段（允许空字符串或 null）
        if (!msg.contains("content")) {
            RCLCPP_WARN(get_logger(), "消息缺少 content 字段");
            return false;
        }

        // 3. 如果已有标准 tool_calls，检查其格式
        if (msg.contains("tool_calls") && !msg["tool_calls"].is_null()) {
            if (!msg["tool_calls"].is_array() && !msg["tool_calls"].is_object()) {
                RCLCPP_WARN(get_logger(), "tool_calls 格式错误（非数组/对象）");
                return false;
            }
            // API 规范：有 tool_calls 时 content 应为空，保留 reasoning_content
            if (msg.contains("content") && msg["content"].is_string())
                msg["content"] = "";
            return true;
        }

        // 4. 如果没有标准 tool_calls，检查是否为 DSML 格式，若是则视为无效消息
        if (role == "assistant" && msg.contains("content") && msg["content"].is_string()) {
            std::string content = msg["content"].get<std::string>();
            if (content.find("<|DSML|tool_calls>") != std::string::npos) {
                RCLCPP_WARN(get_logger(), "检测到 DSML 格式，视为无效消息");
                return false;  // 交给外层替换
            }
        }

        // API 规范：无 tool_calls 时不保留 reasoning_content
        if (role == "assistant")
            msg.erase("reasoning_content");

        return true;
    }

    // ---------- 各角色响应函数 ----------
    bool response_system() {
        RCLCPP_FATAL(get_logger(), "末尾是 system 消息，验证错误！");
        return false;
    }

    bool response_user() {
        json tools = fetch_current_tools();
        int input_tokens = -1;
        json reply = call_llm_raw(tools, &input_tokens);
        current_input_tokens_ = input_tokens;

        if (reply.is_null()) {
            msg_history_.push_back({
                {"role", "assistant"},
                {"content", "[System: LLM 调用失败]"}
            });
        } else {
            // 修复非数组的 tool_calls
            if (reply.contains("tool_calls") && reply["tool_calls"].is_object()) {
                reply["tool_calls"] = json::array({reply["tool_calls"]});
            }
            msg_history_.push_back(reply);
        }
        save_current_json();

        return (current_input_tokens_ > max_context_tokens_);
    }

    bool response_assistant() {
        json& msg = msg_history_.back();

        // 修复非数组 tool_calls
        if (msg.contains("tool_calls") && msg["tool_calls"].is_object()) {
            msg["tool_calls"] = json::array({msg["tool_calls"]});
        }

        if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()) {
            return response_assistant_with_tools();
        } else {
            return response_assistant_no_tools();
        }
    }

    bool response_assistant_no_tools() {
        msg_history_.push_back({
            {"role", "user"},
            {"content", "在本架构中用户无法查看本条回复，选择合适的消息渠道回复，或使用 sleep 5 静默。"}
        });
        save_current_json();
        append_input_snapshot();
        return false;
    }

    bool response_assistant_with_tools() {
        json& msg = msg_history_.back();
        auto tool_calls = msg["tool_calls"];
        std::vector<json> tool_results;
        for (const auto& tc : tool_calls) {
            json tr = execute_single_tool(tc);
            if (!tr.is_null()) tool_results.push_back(tr);
        }
        for (auto& tr : tool_results) {
            msg_history_.push_back(tr);
        }
        save_current_json();
        // 只执行工具，追加结果，不调 LLM，下一轮小循环自然会处理
        return false;
    }

    bool response_tool() {
        RCLCPP_INFO(get_logger(), "末尾出现孤立 tool 消息，追加快照");
        append_input_snapshot();
        return false;
    }

    // ---------- 执行单个工具 ----------
    json execute_single_tool(const json& tc) {
        if (!tc.contains("function")) return {};
        auto func = tc["function"];
        std::string tool_name = func.value("name", "");
        std::string arguments = func.value("arguments", "{}");

        auto goal_msg = ExecuteTool::Goal();
        json goal_input;
        goal_input["name"] = tool_name;
        try {
            goal_input["arguments"] = json::parse(arguments);
        } catch (...) {
            goal_input["arguments"] = arguments;
        }
        goal_msg.input_json = goal_input.dump();

        if (!output_action_client_->wait_for_action_server(std::chrono::seconds(5))) {
            RCLCPP_ERROR(get_logger(), "output action server 不可用，工具 %s 调用失败", tool_name.c_str());
            return {
                {"role", "tool"},
                {"tool_call_id", tc.value("id", "")},
                {"content", "[工具 " + tool_name + "] 错误: action server 不可用"}
            };
        }

        auto send_goal_future = output_action_client_->async_send_goal(goal_msg);
        if (!wait_for_future(send_goal_future, std::chrono::seconds(5))) {
            RCLCPP_ERROR(get_logger(), "发送 goal 超时，工具 %s", tool_name.c_str());
            return {
                {"role", "tool"},
                {"tool_call_id", tc.value("id", "")},
                {"content", "[工具 " + tool_name + "] 错误: 发送 goal 超时"}
            };
        }
        auto goal_handle = send_goal_future.get();
        if (!goal_handle) {
            RCLCPP_ERROR(get_logger(), "goal 被拒绝，工具 %s", tool_name.c_str());
            return {
                {"role", "tool"},
                {"tool_call_id", tc.value("id", "")},
                {"content", "[工具 " + tool_name + "] 错误: goal 被拒绝"}
            };
        }

        auto result_future = output_action_client_->async_get_result(goal_handle);
        if (!wait_for_future(result_future, std::chrono::seconds(120))) {
            RCLCPP_ERROR(get_logger(), "工具执行超时: %s", tool_name.c_str());
            return {
                {"role", "tool"},
                {"tool_call_id", tc.value("id", "")},
                {"content", "[" + tool_name + "] 错误: 执行超时"}
            };
        }
        auto wrapped = result_future.get();

        int exit_code = wrapped.result->exit_code;
        std::string raw_output = wrapped.result->output_json;
        RCLCPP_INFO(get_logger(), "工具 %s 返回: exit_code=%d, output=%s",
                    tool_name.c_str(), exit_code, raw_output.substr(0, 200).c_str());

        std::stringstream ss;
        ss << raw_output;

        std::string content_str = ss.str();
        if (content_str.empty()) content_str = "[no output]";

        return {
            {"role", "tool"},
            {"tool_call_id", tc.value("id", "")},
            {"content", content_str}
        };
    }

    // ---------- 获取当前工具列表 ----------
    json fetch_current_tools() {
        json tools = json::array();
        if (tools_info_client_->wait_for_service(std::chrono::seconds(2))) {
            auto req = std::make_shared<GetToolsInfo::Request>();
            auto future = tools_info_client_->async_send_request(req);
            if (wait_for_future(future, std::chrono::seconds(5))) {
                auto res = future.get();
                try {
                    tools = json::parse(res->tools_json);
                } catch (...) {
                    RCLCPP_WARN(get_logger(), "工具列表 JSON 解析失败");
                }
            } else {
                RCLCPP_WARN(get_logger(), "获取工具列表超时");
            }
        } else {
            RCLCPP_WARN(get_logger(), "tools_info 服务不可用");
        }
        return tools;
    }

    // ---------- 纯粹的 LLM 调用 ----------
    json call_llm_raw(const json& tools, int* out_input_tokens) {
        json reply;
        try {
            openai_client_->clear_messages();
            for (const auto& m : msg_history_) {
                openai_client_->add_message(m);
            }
            reply = openai_client_->call_api(false, tools, out_input_tokens);
        } catch (const std::exception& e) {
            RCLCPP_ERROR(get_logger(), "LLM 调用异常: %s", e.what());
            if (out_input_tokens) *out_input_tokens = -1;
            return {};
        }
        if (reply.is_null() || !reply.contains("role")) {
            return {};
        }
        return reply;
    }

    // ---------- 追加快照 ----------
    bool append_input_snapshot() {
        if (!snapshot_client_->wait_for_service(std::chrono::seconds(2))) {
            RCLCPP_WARN(get_logger(), "input snapshot 服务不可用，跳过");
            msg_history_.push_back({{"role", "user"}, {"content", "{}"}});
            save_current_json();
            return true;
        }
        auto req = std::make_shared<GetSnapshot::Request>();
        auto future = snapshot_client_->async_send_request(req);
        std::string snapshot_str = "{}";
        if (wait_for_future(future, std::chrono::seconds(5))) {
            auto res = future.get();
            snapshot_str = res->snapshot_json;
        } else {
            RCLCPP_WARN(get_logger(), "获取快照超时");
        }
        // 压缩后首次快照：先发一条独立的上下文恢复提示，再发快照
        if (needs_post_compress_macro_) {
            needs_post_compress_macro_ = false;
            msg_history_.push_back({{"role", "user"}, {"content", "上下文已压缩。上方概要包含了压缩前内容的摘要，请自然地继续对话。"}});
        }
        msg_history_.push_back({{"role", "user"}, {"content", snapshot_str}});
        save_current_json();
        return true;
    }

    // ---------- 安全等待 future ----------
    template<typename FutureT>
    bool wait_for_future(FutureT& future, std::chrono::nanoseconds timeout) {
        auto start = std::chrono::steady_clock::now();
        while (rclcpp::ok()) {
            if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
                return true;
            }
            if (std::chrono::steady_clock::now() - start > timeout) {
                return false;
            }
            rclcpp::spin_some(this->shared_from_this());
        }
        return false;
    }

    // ---------- 成员变量 ----------
    std::string agent_name_;
    std::string context_dir_;
    int max_context_tokens_;
    int summary_turns_;
    json msg_history_;
    std::string current_json_path_;
    int current_input_tokens_ = 0;
    bool needs_post_compress_macro_ = false;

    std::unique_ptr<openai_client::OpenAIClient> openai_client_;

    rclcpp::Client<MemoryRecall>::SharedPtr memory_recall_client_;
    rclcpp::Client<MemoryArchive>::SharedPtr memory_archive_client_;
    rclcpp::Client<GetSnapshot>::SharedPtr snapshot_client_;
    rclcpp::Client<GetToolsInfo>::SharedPtr tools_info_client_;

    rclcpp_action::Client<ExecuteTool>::SharedPtr output_action_client_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);

    auto temp = std::make_shared<rclcpp::Node>("temp");
    temp->declare_parameter<std::string>("agent_name", "agent");
    std::string agent_name = temp->get_parameter("agent_name").as_string();
    temp.reset();

    auto node = std::make_shared<AgentLoopNode>(agent_name);
    node->run();

    rclcpp::shutdown();
    return 0;
}