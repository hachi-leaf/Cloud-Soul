// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// =============================================================================
// 节点: <agent_name>_agent_loop
// 作用: Agent 主循环，执行感知 -> 决策 -> 行动 的闭环。
//
// 架构简述:
// 1. 初始化：等待依赖服务上线，加载记忆系统提示词，恢复或创建上下文。
// 2. 主循环（若 loop_rate=0 则连续循环，否则按频率触发）：
//    a. 获取输入快照（GetInputSnapshot），作为 user 消息加入历史，立即保存上下文。
//    b. 获取可用工具列表（GetToolsInfo）。
//    c. 思考-工具调用小循环：调用 LLM，无限重试直至成功。
//       - 若返回 tool_calls：执行工具（通过 /agent_name/output 动作），
//         将工具结果加入历史，继续循环。每次 LLM 回复或工具结果都会立即保存上下文。
//       - 若返回普通 content：退出小循环。
//    d. 检查上下文 token 数，若超过 max_context_tokens 则触发压缩：
//       - 保存当前完整上下文为 JSON 文件。
//       - 调用 memory_archive 归档（失败则反复重试）。
//       - 提取最后 summary_turns 轮对话生成摘要，重新获取系统提示词，重建上下文，立即保存。
//    e. 持久化当前上下文到文件（用于断点恢复）。
//    f. 循环控制：按 loop_rate 等待或立即开始下一轮。
//
// 参数:
//   agent_name          - 命名空间前缀，默认 "agent"
//   context_dir         - 上下文持久化目录，默认 "~/.cloud_soul/contexts"
//   max_context_tokens  - 触发压缩的 token 阈值，默认 32000
//   summary_turns       - 压缩时保留的最近对话轮数，默认 3
//   loop_rate           - 主循环频率 (Hz)，0 表示无间隔连续循环，默认 0
//   openai_base_url     - OpenAI API 基础 URL，默认 "https://api.deepseek.com"
//   openai_api_key      - API 密钥，默认取环境变量 OPENAI_API_KEY
//   openai_model        - 模型名称，默认 "deepseek-v4-pro"
//
// 发布/订阅: 无直接发布/订阅，通过服务客户端和动作客户端与其他节点交互。
// =============================================================================

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <cs_interfaces/srv/get_input_snapshot.hpp>
#include <cs_interfaces/srv/get_tools_info.hpp>
#include <cs_interfaces/srv/memory_recall.hpp>
#include <cs_interfaces/srv/memory_archive.hpp>
#include <cs_interfaces/action/execute_tool.hpp>
#include <cs_core/call_openai.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <deque>
#include <ctime>
#include <mutex>
#include <future>
#include <algorithm>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

using GetInputSnapshot = cs_interfaces::srv::GetInputSnapshot;
using GetToolsInfo = cs_interfaces::srv::GetToolsInfo;
using MemoryRecall = cs_interfaces::srv::MemoryRecall;
using MemoryArchive = cs_interfaces::srv::MemoryArchive;
using ExecuteTool = cs_interfaces::action::ExecuteTool;

class AgentLoopNode : public rclcpp::Node {
public:
  AgentLoopNode() : Node("agent_loop_node") {
    this->declare_parameter<std::string>("agent_name", "");
    this->declare_parameter<std::string>("context_dir", "~/.cloud_soul/contexts");
    this->declare_parameter<int>("max_context_tokens", 32000);
    this->declare_parameter<int>("summary_turns", 3);
    this->declare_parameter<double>("loop_rate", 0.0);
    this->declare_parameter<std::string>("openai_base_url", "https://api.deepseek.com");
    this->declare_parameter<std::string>("openai_api_key", "");
    this->declare_parameter<std::string>("openai_model", "deepseek-v4-pro");

    agent_name_ = this->get_parameter("agent_name").as_string();
    if (agent_name_.empty()) {
      RCLCPP_FATAL(this->get_logger(), "agent_name 参数不能为空");
      rclcpp::shutdown();
      return;
    }

    std::string raw_dir = this->get_parameter("context_dir").as_string();
    if (!raw_dir.empty() && raw_dir[0] == '~') {
      const char* home = std::getenv("HOME");
      if (home) raw_dir = std::string(home) + raw_dir.substr(1);
    }
    context_dir_ = raw_dir;
    if (!fs::exists(context_dir_)) fs::create_directories(context_dir_);

    // 锁定已有的上下文文件（后缀 _agent_name.json）
    context_file_path_ = find_context_file();
    if (context_file_path_.empty()) {
      RCLCPP_INFO(this->get_logger(), "未找到已有上下文文件，将创建新文件");
      context_file_path_ = create_new_context_filename();
    } else {
      RCLCPP_INFO(this->get_logger(), "恢复上下文文件: %s", context_file_path_.c_str());
    }

    max_context_tokens_ = this->get_parameter("max_context_tokens").as_int();
    summary_turns_ = this->get_parameter("summary_turns").as_int();
    loop_rate_ = this->get_parameter("loop_rate").as_double();
    openai_base_url_ = this->get_parameter("openai_base_url").as_string();
    openai_api_key_ = this->get_parameter("openai_api_key").as_string();
    openai_model_ = this->get_parameter("openai_model").as_string();

    if (openai_api_key_.empty()) {
      const char* env = std::getenv("OPENAI_API_KEY");
      if (env) openai_api_key_ = env;
    }
    if (openai_api_key_.empty()) {
      RCLCPP_FATAL(this->get_logger(), "无法获取 API key");
      rclcpp::shutdown();
      return;
    }

    input_client_ = this->create_client<GetInputSnapshot>("/" + agent_name_ + "/input");
    tools_client_ = this->create_client<GetToolsInfo>("/" + agent_name_ + "/output/info");
    recall_client_ = this->create_client<MemoryRecall>("/" + agent_name_ + "/memory_recall");
    archive_client_ = this->create_client<MemoryArchive>("/" + agent_name_ + "/memory_archive");
    action_client_ = rclcpp_action::create_client<ExecuteTool>(this, "/" + agent_name_ + "/output");

    RCLCPP_INFO(this->get_logger(), "Agent 循环节点启动，agent: %s", agent_name_.c_str());
    loop_thread_ = std::thread(&AgentLoopNode::run_loop, this);
  }

  ~AgentLoopNode() {
    if (loop_thread_.joinable()) loop_thread_.join();
  }

private:
  // 查找目录下匹配 *_<agent_name>.json 的文件，返回最新修改的文件路径
  std::string find_context_file() {
    std::string found;
    // 关键修复：初始化为最小值，确保任何文件的修改时间都更大
    fs::file_time_type latest_ftime = fs::file_time_type::min();
    std::string suffix = "_" + agent_name_ + ".json";
    RCLCPP_INFO(this->get_logger(), "在 %s 中查找后缀为 '%s' 的文件",
                context_dir_.c_str(), suffix.c_str());
    for (const auto& entry : fs::directory_iterator(context_dir_)) {
      if (!entry.is_regular_file()) continue;
      std::string fname = entry.path().filename().string();
      if (fname.size() < suffix.size()) continue;
      if (fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
      auto ftime = entry.last_write_time();
      if (ftime > latest_ftime) {
        latest_ftime = ftime;
        found = entry.path().string();
      }
    }
    if (!found.empty()) {
      RCLCPP_INFO(this->get_logger(), "找到上下文文件: %s", found.c_str());
    } else {
      RCLCPP_INFO(this->get_logger(), "目录中无匹配 %s 的文件", suffix.c_str());
    }
    return found;
  }

  // 按规范创建新文件名：Z_YYYYMMDD_HHMMSS_<agent_name>.json
  std::string create_new_context_filename() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm* gmt = std::gmtime(&now_t);
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", gmt);
    std::string filename = "Z_" + std::string(ts_buf) + "_" + agent_name_ + ".json";
    return context_dir_ + "/" + filename;
  }

  template <typename T>
  void wait_for_service(typename rclcpp::Client<T>::SharedPtr client, const std::string& name) {
    RCLCPP_DEBUG(this->get_logger(), "等待服务 %s 上线...", name.c_str());
    while (rclcpp::ok() && !client->wait_for_service(2s)) {
      RCLCPP_DEBUG(this->get_logger(), "服务 %s 仍未就绪，重试中...", name.c_str());
    }
    RCLCPP_DEBUG(this->get_logger(), "服务 %s 已上线", name.c_str());
  }

  template <typename ServiceT>
  typename ServiceT::Response::SharedPtr call_service_reliably(
      typename rclcpp::Client<ServiceT>::SharedPtr client,
      const std::string& name) {
    while (rclcpp::ok()) {
      auto req = std::make_shared<typename ServiceT::Request>();
      auto future = client->async_send_request(req);
      if (future.wait_for(5s) == std::future_status::ready) {
        auto resp = future.get();
        if (resp) return resp;
      }
      RCLCPP_WARN(this->get_logger(), "调用服务 %s 失败，重试中...", name.c_str());
      std::this_thread::sleep_for(1s);
    }
    return nullptr;
  }

  std::string load_rule_text() {
    auto resp = call_service_reliably<MemoryRecall>(recall_client_, "memory_recall");
    if (!resp) {
      RCLCPP_ERROR(this->get_logger(), "memory_recall 调用失败，使用空规则");
      return "";
    }
    if (resp->error_code != 0) {
      RCLCPP_WARN(this->get_logger(), "memory_recall 返回错误码 %d: %s", resp->error_code, resp->text.c_str());
      return resp->text;
    }
    return resp->text;
  }

  void restore_or_initialize_messages() {
    if (fs::exists(context_file_path_)) {
      std::ifstream ifs(context_file_path_);
      if (ifs) {
        try {
          auto j = nlohmann::json::parse(ifs);
          if (j.is_array()) {
            messages_ = j.get<std::vector<nlohmann::json>>();
            RCLCPP_INFO(this->get_logger(), "从 %s 恢复上下文，包含 %zu 条消息",
                        context_file_path_.c_str(), messages_.size());
            return;
          }
        } catch (...) {
          RCLCPP_WARN(this->get_logger(), "上下文文件解析失败，将重新初始化");
        }
      }
    }

    // 新建上下文
    std::string rule = load_rule_text();
    messages_.clear();
    messages_.push_back({{"role", "system"}, {"content", rule}});
    RCLCPP_INFO(this->get_logger(), "初始化新上下文");
    save_context();
  }

  void save_context() {
    std::ofstream ofs(context_file_path_);
    if (!ofs) {
      RCLCPP_ERROR(this->get_logger(), "无法写入上下文文件 %s", context_file_path_.c_str());
      return;
    }
    // 每条消息紧凑一行，消息之间换行
    ofs << "[\n";
    for (size_t i = 0; i < messages_.size(); ++i) {
      if (i > 0) ofs << ",\n";
      ofs << messages_[i].dump();
    }
    ofs << "\n]";
    ofs.close();
    RCLCPP_DEBUG(this->get_logger(), "上下文已保存至 %s (%zu 条消息)",
                 context_file_path_.c_str(), messages_.size());
  }

  int estimate_tokens(const std::vector<nlohmann::json>& msgs) {
    std::size_t total_chars = 0;
    for (const auto& m : msgs) {
      total_chars += m.dump().size();
    }
    return static_cast<int>(total_chars / 4);
  }

  void compress_context() {
    RCLCPP_INFO(this->get_logger(), "上下文 token 超限，开始压缩...");
    save_context();

    // 归档
    while (rclcpp::ok()) {
      auto req = std::make_shared<MemoryArchive::Request>();
      req->json_path = context_file_path_;
      auto future = archive_client_->async_send_request(req);
      if (future.wait_for(5s) == std::future_status::ready) {
        auto resp = future.get();
        if (resp && resp->error_code == 0) {
          RCLCPP_INFO(this->get_logger(), "上下文归档成功");
          break;
        } else {
          RCLCPP_WARN(this->get_logger(), "上下文归档失败 (error_code=%d)，重试中...",
                      resp ? resp->error_code : -1);
        }
      } else {
        RCLCPP_WARN(this->get_logger(), "归档服务调用超时，重试中...");
      }
      std::this_thread::sleep_for(2s);
    }

    // 生成摘要
    std::string summary;
    std::vector<std::pair<std::string, std::string>> turns;
    std::string last_user;
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
      if (it->value("role", "") == "user") {
        last_user = it->value("content", "");
      } else if (it->value("role", "") == "assistant" && !last_user.empty()) {
        std::string assistant = it->value("content", "");
        if (!assistant.empty()) {
          turns.push_back({last_user, assistant});
          last_user.clear();
          if (turns.size() >= static_cast<size_t>(summary_turns_)) break;
        }
      }
    }
    for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
      summary += "用户: " + it->first + "\n";
      summary += "助手: " + it->second + "\n\n";
    }

    std::string rule = load_rule_text();
    messages_.clear();
    messages_.push_back({{"role", "system"}, {"content", rule + "\n\n[对话摘要]\n" + summary}});
    RCLCPP_INFO(this->get_logger(), "上下文压缩完成，重新开始");
    save_context();
  }

  // 清理字符串中的控制字符
  static std::string sanitize_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\r' && c != '\t') {
        out += "\\u00";
        out += "0123456789abcdef"[(c >> 4) & 0xf];
        out += "0123456789abcdef"[c & 0xf];
      } else {
        out += c;
      }
    }
    return out;
  }

  // 递归清理 nlohmann::json 对象中所有字符串值
  static void sanitize_json_recursive(nlohmann::json& obj) {
    if (obj.is_string()) {
      obj = sanitize_json_string(obj.get<std::string>());
    } else if (obj.is_array()) {
      for (auto& item : obj) {
        sanitize_json_recursive(item);
      }
    } else if (obj.is_object()) {
      for (auto& [key, value] : obj.items()) {
        sanitize_json_recursive(value);
      }
    }
  }

  nlohmann::json execute_tool(const std::string& tool_name, const std::string& arguments_json) {
    auto goal = ExecuteTool::Goal();
    nlohmann::json call_obj = {{"name", tool_name}, {"arguments", nlohmann::json::parse(arguments_json)}};
    goal.input_json = call_obj.dump();

    auto send_goal = action_client_->async_send_goal(goal);
    if (send_goal.wait_for(5s) != std::future_status::ready) {
      return {{"error", "工具调用超时"}};
    }
    auto goal_handle = send_goal.get();
    if (!goal_handle) {
      return {{"error", "目标被拒绝"}};
    }
    auto result_future = action_client_->async_get_result(goal_handle);
    if (result_future.wait_for(10s) != std::future_status::ready) {
      return {{"error", "工具执行超时"}};
    }
    auto result = result_future.get();
    if (result.code != rclcpp_action::ResultCode::SUCCEEDED) {
      return {{"error", "工具执行失败"}};
    }
    return nlohmann::json::parse(result.result->output_json);
  }

  nlohmann::json clean_message_for_api(const nlohmann::json& msg) {
    if (msg.value("role", "") != "assistant") return msg;
    auto cleaned = msg;
    bool has_tool_calls = cleaned.contains("tool_calls") && !cleaned["tool_calls"].is_null() &&
                          cleaned["tool_calls"].is_array() && !cleaned["tool_calls"].empty();
    if (!has_tool_calls) {
      cleaned.erase("reasoning_content");
    }
    return cleaned;
  }

  nlohmann::json call_llm_reliably(openai_client::OpenAIClient& client,
                                   const nlohmann::json& tools) {
    while (rclcpp::ok()) {
      try {
        return client.call_api(false, tools);
      } catch (const std::exception& e) {
        RCLCPP_WARN(this->get_logger(), "LLM 调用失败: %s，重试中...", e.what());
        std::this_thread::sleep_for(2s);
      }
    }
    return {};
  }

  void run_loop() {
    RCLCPP_INFO(this->get_logger(), "开始 Agent 主循环");

    wait_for_service<GetInputSnapshot>(input_client_, "input");
    wait_for_service<GetToolsInfo>(tools_client_, "output/info");
    wait_for_service<MemoryRecall>(recall_client_, "memory_recall");
    wait_for_service<MemoryArchive>(archive_client_, "memory_archive");
    if (!action_client_->wait_for_action_server(2s)) {
      RCLCPP_WARN(this->get_logger(), "输出动作服务器未就绪，将在工具调用时重试");
    }

    restore_or_initialize_messages();

    while (rclcpp::ok()) {
      auto input_resp = call_service_reliably<GetInputSnapshot>(input_client_, "input");
      if (input_resp) {
        std::string snapshot = input_resp->snapshot_json;
        if (!snapshot.empty()) {
          messages_.push_back({{"role", "user"}, {"content", snapshot}});
          RCLCPP_DEBUG(this->get_logger(), "添加输入快照到历史");
          save_context();
        }
      }

      auto tools_resp = call_service_reliably<GetToolsInfo>(tools_client_, "output/info");
      nlohmann::json tools = nlohmann::json::array();
      if (tools_resp) {
        try {
          tools = nlohmann::json::parse(tools_resp->tools_json);
          if (!tools.is_array()) tools = nlohmann::json::array();
        } catch (const std::exception& e) {
          RCLCPP_WARN(this->get_logger(), "解析 tools 失败: %s", e.what());
        }
      }

      openai_client::OpenAIClient client(openai_base_url_, openai_api_key_, openai_model_);
      client.clear_messages();
      for (const auto& m : messages_) {
        client.add_message(clean_message_for_api(m));
      }

      while (rclcpp::ok()) {
        auto reply = call_llm_reliably(client, tools);
        sanitize_json_recursive(reply);
        messages_.push_back(reply);
        client.add_message(reply);
        save_context();

        bool has_tool_calls = reply.contains("tool_calls") && !reply["tool_calls"].is_null() &&
                              reply["tool_calls"].is_array() && !reply["tool_calls"].empty();
        if (!has_tool_calls) {
          break;
        }

        for (const auto& tc : reply["tool_calls"]) {
          std::string func_name = tc["function"]["name"];
          std::string arguments = tc["function"]["arguments"];
          std::string tool_id = tc.value("id", "");
          RCLCPP_INFO(this->get_logger(), "调用工具: %s", func_name.c_str());

          nlohmann::json tool_result = execute_tool(func_name, arguments);
          sanitize_json_recursive(tool_result);
          nlohmann::json tool_msg = {
            {"role", "tool"},
            {"tool_call_id", tool_id},
            {"content", tool_result.dump()}
          };
          messages_.push_back(tool_msg);
          client.add_message(tool_msg);
          save_context();
        }
      }

      if (estimate_tokens(messages_) >= max_context_tokens_) {
        compress_context();
      }

      if (loop_rate_ > 0.0) {
        std::chrono::duration<double> interval(1.0 / loop_rate_);
        std::this_thread::sleep_for(interval);
      }
    }
  }

  std::string agent_name_;
  std::string context_dir_;
  std::string context_file_path_;
  int max_context_tokens_;
  int summary_turns_;
  double loop_rate_;
  std::string openai_base_url_;
  std::string openai_api_key_;
  std::string openai_model_;

  rclcpp::Client<GetInputSnapshot>::SharedPtr input_client_;
  rclcpp::Client<GetToolsInfo>::SharedPtr tools_client_;
  rclcpp::Client<MemoryRecall>::SharedPtr recall_client_;
  rclcpp::Client<MemoryArchive>::SharedPtr archive_client_;
  rclcpp_action::Client<ExecuteTool>::SharedPtr action_client_;

  std::vector<nlohmann::json> messages_;
  std::thread loop_thread_;
};

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<AgentLoopNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}