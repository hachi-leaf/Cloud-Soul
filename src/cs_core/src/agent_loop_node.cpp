// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// =============================================================================
// 节点: /<agent_name>/agent_loop
// 作用: Agent 主循环，执行感知 -> 决策 -> 行动 的无限思考闭环。
//
// 上下文拼接规范:
//   [system] 记忆系统提示词
//   [assistant] 主动思考 / 工具调用 ...  ← 零号思考，不依赖快照
//   [user] 工具返回 (OpenAI role: tool)
//   [user] 输入快照
//   [assistant] 思考 / 工具调用 ...
//   [user] 工具返回
//   [user] 输入快照
//   ... 无限循环，每一轮 assistant 回复后都获取快照并作为 user 消息插入
//
// 修复说明:
//   - assistant 消息包含 tool_calls 时，content 强制设为 null
//   - 发送给 API 的消息统一保留删除 reasoning_content
//   - 每次 LLM 调用后立即获取快照，确保模型始终看到最新外部输入
//   - tool_msg["content"] 使用 result.dump() 存为字符串
//   - 工具参数 JSON 解析增加 try-catch，防止 LLM 错误参数导致崩溃
//   - save_context 增加异常保护
//
// 参数:
//   agent_name          - 命名空间前缀
//   context_dir         - 上下文持久化目录
//   max_context_tokens  - 触发压缩的 token 阈值
//   summary_turns       - 压缩时保留的最近对话轮数
//   loop_rate           - 主循环频率 (Hz)，0 为无间隔
//   openai_base_url/openai_api_key/openai_model - LLM 配置
// =============================================================================

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include "std_msgs/msg/string.hpp"
#include <cs_interfaces/srv/get_snapshot.hpp>
#include <cs_interfaces/srv/get_tools_info.hpp>
#include <cs_interfaces/srv/memory_recall.hpp>
#include <cs_interfaces/srv/memory_archive.hpp>
#include <cs_interfaces/action/execute_tool.hpp>
#include <cs_core/call_openai.hpp>
#include <cs_interfaces/constants.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <deque>
#include <ctime>
#include <algorithm>
#include <functional>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

using GetSnapshot   = cs_interfaces::srv::GetSnapshot;
using GetToolsInfo  = cs_interfaces::srv::GetToolsInfo;
using MemoryRecall  = cs_interfaces::srv::MemoryRecall;
using MemoryArchive = cs_interfaces::srv::MemoryArchive;
using ExecuteTool   = cs_interfaces::action::ExecuteTool;



class AgentLoopNode : public rclcpp::Node {
public:
  explicit AgentLoopNode(const rclcpp::NodeOptions & options = rclcpp::NodeOptions())
    : Node("agent_loop", options)
  {
    // 参数声明
    agent_name_   = declare_parameter<std::string>("agent_name", "");
    std::string raw_dir = declare_parameter<std::string>("context_dir", "~/.cloudsoul/contexts");
    max_context_tokens_ = declare_parameter<int>("max_context_tokens", 200000);
    summary_turns_      = declare_parameter<int>("summary_turns", 30);
    loop_rate_          = declare_parameter<double>("loop_rate", 0.0);
    tool_timeout_duration_ = std::chrono::duration<double>(declare_parameter<double>("tool_timeout", 60.0));
    openai_base_url_    = declare_parameter<std::string>("openai_base_url", "https://api.deepseek.com");
    openai_api_key_     = declare_parameter<std::string>("openai_api_key", "");
    openai_model_       = declare_parameter<std::string>("openai_model", "deepseek-chat");

    if (agent_name_.empty()) {
      RCLCPP_FATAL(get_logger(), "agent_name 参数不能为空");
      rclcpp::shutdown();
      return;
    }

    if (!raw_dir.empty() && raw_dir[0] == '~') {
      const char* home = std::getenv("HOME");
      if (home) raw_dir = std::string(home) + raw_dir.substr(1);
    }
    context_dir_ = raw_dir;
    if (!fs::exists(context_dir_)) fs::create_directories(context_dir_);

    context_file_path_ = find_context_file();
    if (context_file_path_.empty()) {
      context_file_path_ = create_new_context_filename();
      RCLCPP_INFO(get_logger(), "新建上下文文件: %s", context_file_path_.c_str());
    } else {
      RCLCPP_INFO(get_logger(), "恢复上下文文件: %s", context_file_path_.c_str());
    }

    if (openai_api_key_.empty()) {
      const char* env = std::getenv("OPENAI_API_KEY");
      if (env) openai_api_key_ = env;
    }
    if (openai_api_key_.empty()) {
      RCLCPP_FATAL(get_logger(), "无法获取 API key");
      rclcpp::shutdown();
      return;
    }

    input_client_   = create_client<GetSnapshot>("/" + agent_name_ + "/input");
    tools_client_   = create_client<GetToolsInfo>("/" + agent_name_ + "/output/info");
    recall_client_  = create_client<MemoryRecall>("/" + agent_name_ + "/memory_recall");
    archive_client_ = create_client<MemoryArchive>("/" + agent_name_ + "/memory_archive");
    action_client_  = rclcpp_action::create_client<ExecuteTool>(this, "/" + agent_name_ + "/output");
    response_pub_   = create_publisher<std_msgs::msg::String>("/" + agent_name_ + "/response", 10);

    RCLCPP_INFO(get_logger(), "Agent 循环节点启动，agent: %s", agent_name_.c_str());
    loop_thread_ = std::thread(&AgentLoopNode::run_loop, this);
  }

  ~AgentLoopNode() override {
    if (loop_thread_.joinable()) loop_thread_.join();
  }

private:
  // ---------------------------------------------------------------
  // 工具方法
  // ---------------------------------------------------------------
  std::string find_context_file() {
    std::string found;
    fs::file_time_type latest = fs::file_time_type::min();
    std::string suffix = "_" + agent_name_ + ".json";
    for (const auto& entry : fs::directory_iterator(context_dir_)) {
      if (!entry.is_regular_file()) continue;
      std::string fname = entry.path().filename().string();
      if (fname.size() < suffix.size()) continue;
      if (fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
      auto ftime = entry.last_write_time();
      if (ftime > latest) {
        latest = ftime;
        found = entry.path().string();
      }
    }
    return found;
  }

  std::string create_new_context_filename() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm* gmt = std::gmtime(&tt);
    char buf[cloud_soul::TIMESTAMP_BUF_SIZE];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", gmt);
    return context_dir_ + "/Z_" + buf + "_" + agent_name_ + ".json";
  }

  template <typename T>
  void wait_for_service(typename rclcpp::Client<T>::SharedPtr client, const std::string& name) {
    while (rclcpp::ok() && !client->wait_for_service(cloud_soul::SERVICE_WAIT_TIMEOUT)) {
      std::this_thread::sleep_for(cloud_soul::SERVICE_RETRY_INTERVAL);
    }
  }

  template <typename ServiceT>
  typename ServiceT::Response::SharedPtr call_service_reliably(
      typename rclcpp::Client<ServiceT>::SharedPtr client, const std::string& /*name*/) {
    while (rclcpp::ok()) {
      auto req = std::make_shared<typename ServiceT::Request>();
      auto future = client->async_send_request(req);
      if (future.wait_for(cloud_soul::ACTION_REQUEST_TIMEOUT) == std::future_status::ready) {
        auto resp = future.get();
        if (resp) return resp;
      }
      std::this_thread::sleep_for(cloud_soul::SERVICE_RETRY_INTERVAL);
    }
    return nullptr;
  }

  std::string load_rule_text() {
    auto resp = call_service_reliably<MemoryRecall>(recall_client_, "memory_recall");
    if (!resp || resp->error_code != 0) {
      RCLCPP_WARN(get_logger(), "memory_recall 失败，使用空规则");
      return resp ? resp->text : "";
    }
    return resp->text;
  }

  // UTF-8 清理：将无效字节替换为 U+FFFD
  static std::string sanitize_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
      unsigned char c = static_cast<unsigned char>(s[i]);
      size_t len = 0;
      if ((c & 0x80) == 0) {
        len = 1;
      } else if ((c & 0xE0) == 0xC0) {
        len = 2;
      } else if ((c & 0xF0) == 0xE0) {
        len = 3;
      } else if ((c & 0xF8) == 0xF0) {
        len = 4;
      } else {
        len = 0;
      }
      if (len >= 2 && i + len <= s.size()) {
        bool valid = true;
        for (size_t j = 1; j < len; ++j) {
          if ((static_cast<unsigned char>(s[i+j]) & 0xC0) != 0x80) { valid = false; break; }
        }
        if (valid) {
          out.append(s, i, len);
          i += len;
          continue;
        }
      }
      out.append("ï¿½"); // U+FFFD
      i += 1;
    }
    return out;
  }

  static void sanitize_json_utf8(nlohmann::json& j) {
    if (j.is_string()) {
      j = sanitize_utf8(j.get<std::string>());
    } else if (j.is_array()) {
      for (auto& elem : j) sanitize_json_utf8(elem);
    } else if (j.is_object()) {
      for (auto& [k, v] : j.items()) sanitize_json_utf8(v);
    }
  }

  void save_context() {
    try {
      std::ofstream ofs(context_file_path_);
      if (!ofs) {
        RCLCPP_ERROR(get_logger(), "写入上下文文件失败: %s", context_file_path_.c_str());
        return;
      }
      ofs << "[\n";
      for (size_t i = 0; i < messages_.size(); ++i) {
        if (i > 0) ofs << ",\n";
        ofs << messages_[i].dump();
      }
      ofs << "\n]";
    } catch (const std::exception& e) {
      RCLCPP_ERROR(get_logger(), "保存上下文时发生异常: %s", e.what());
      // UTF-8 清理后重试一次
      try {
        for (auto& msg : messages_) {
          sanitize_json_utf8(msg);
        }
        std::ofstream ofs2(context_file_path_);
        if (ofs2) {
          ofs2 << "[\n";
          for (size_t i = 0; i < messages_.size(); ++i) {
            if (i > 0) ofs2 << ",\n";
            ofs2 << messages_[i].dump();
          }
          ofs2 << "\n]";
          RCLCPP_INFO(get_logger(), "UTF-8 清理后保存成功");
        }
      } catch (const std::exception& e2) {
        RCLCPP_ERROR(get_logger(), "UTF-8 清理后仍失败: %s", e2.what());
      }
    }
  }

  int estimate_tokens(const std::vector<nlohmann::json>& msgs) {
    size_t total = 0;
    for (const auto& m : msgs) total += m.dump().size();
    return static_cast<int>(total / cloud_soul::TOKEN_ESTIMATE_DIVISOR);
  }

  // ---------------------------------------------------------------
  // 消息清理
  // ---------------------------------------------------------------
  static std::string sanitize_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (c < 0x20) {
        if (c == '\n' || c == '\r' || c == '\t') out += c;
        else { out += "\\u00"; out += "0123456789abcdef"[(c>>4)&0xf]; out += "0123456789abcdef"[c&0xf]; }
        ++i;
      } else if (c == 0x7F) {
        out += "\\u007f"; ++i;
      } else if (c < 0x80) {
        out += c; ++i;
      } else if (c >= 0xC2 && c <= 0xDF) {
        if (i+1 < s.size() && (static_cast<unsigned char>(s[i+1]) & 0xC0) == 0x80) { out += c; out += s[i+1]; i+=2; }
        else { out += "\\u00"; out += "0123456789abcdef"[(c>>4)&0xf]; out += "0123456789abcdef"[c&0xf]; ++i; }
      } else if (c >= 0xE0 && c <= 0xEF) {
        if (i+2 < s.size() && (static_cast<unsigned char>(s[i+1]) & 0xC0) == 0x80 && (static_cast<unsigned char>(s[i+2]) & 0xC0) == 0x80)
        { out += c; out += s[i+1]; out += s[i+2]; i+=3; }
        else { out += "\\u00"; out += "0123456789abcdef"[(c>>4)&0xf]; out += "0123456789abcdef"[c&0xf]; ++i; }
      } else if (c >= 0xF0 && c <= 0xF4) {
        if (i+3 < s.size() && (static_cast<unsigned char>(s[i+1]) & 0xC0) == 0x80 &&
            (static_cast<unsigned char>(s[i+2]) & 0xC0) == 0x80 && (static_cast<unsigned char>(s[i+3]) & 0xC0) == 0x80)
        { out += c; out += s[i+1]; out += s[i+2]; out += s[i+3]; i+=4; }
        else { out += "\\u00"; out += "0123456789abcdef"[(c>>4)&0xf]; out += "0123456789abcdef"[c&0xf]; ++i; }
      } else {
        out += "\\u00"; out += "0123456789abcdef"[(c>>4)&0xf]; out += "0123456789abcdef"[c&0xf]; ++i;
      }
    }
    return out;
  }

  // ---------------------------------------------------------------
  // JSON 参数修复: LLM 生成的 arguments 可能内嵌未转义的引号
  // 状态机: 跟踪 JSON 字符串边界, 对字符串内的裸引号进行转义
  // ---------------------------------------------------------------
  static std::string repair_json_args(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    enum State { DEFAULT, IN_STRING } state = DEFAULT;

    for (size_t i = 0; i < s.size(); ++i) {
      char c = s[i];

      if (state == DEFAULT) {
        if (c == '"') state = IN_STRING;
        out += c;
      } else { // IN_STRING
        if (c == '\\' && i+1 < s.size()) {
          out += c;
          out += s[++i];  // preserve escape
        } else if (c == '"') {
          // look ahead: is this a string terminator?
          size_t j = i + 1;
          while (j < s.size()
                 && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r'))
            ++j;
          if (j < s.size()
              && (s[j] == ',' || s[j] == '}' || s[j] == ']' || s[j] == ':')) {
            state = DEFAULT;  // real terminator
            out += c;
          } else {
            out += "\\\"";  // content quote, escape it
          }
        } else {
          out += c;
        }
      }
    }
    return out;
  }
  static void sanitize_json_recursive(nlohmann::json& j) {
    if (j.is_string()) j = sanitize_json_string(j.get<std::string>());
    else if (j.is_array()) for (auto& e : j) sanitize_json_recursive(e);
    else if (j.is_object()) for (auto& [k, v] : j.items()) sanitize_json_recursive(v);
  }

  // ---------------------------------------------------------------
  // 恢复未完成的工具调用
  // ---------------------------------------------------------------
  bool recover_incomplete_tool_call() {
    if (messages_.empty()) return false;
    auto& last = messages_.back();
    if (last.value("role", "") != "assistant") return false;
    if (!last.contains("tool_calls") || !last["tool_calls"].is_array() || last["tool_calls"].empty())
      return false;

    RCLCPP_WARN(get_logger(), cloud_soul::Msg::INCOMPLETE_TOOL_FOUND);

    for (const auto& tc : last["tool_calls"]) {
      std::string func = tc["function"]["name"];
      std::string args = tc["function"]["arguments"];
      std::string id   = tc.value("id", "");

      nlohmann::json result = execute_tool(func, args);
      sanitize_json_recursive(result);
      nlohmann::json tool_msg = {
        {"role", "tool"},
        {"tool_call_id", id},
        {"content", result.dump()}
      };
      messages_.push_back(tool_msg);

      if (func == "user_notify") {
        try {
          auto a = nlohmann::json::parse(args);
          if (a.contains("message") && !a["message"].is_null()) {
            auto msg = std_msgs::msg::String();
            msg.data = a["message"].get<std::string>();
            response_pub_->publish(msg);
          }
        } catch (...) {}
      }
    }
    save_context();
    return true;
  }

  // ---------------------------------------------------------------
  // 初始化消息列表
  // ---------------------------------------------------------------
  void initialize_messages() {
    if (fs::exists(context_file_path_)) {
      std::ifstream ifs(context_file_path_);
      if (ifs) {
        try {
          // 读入原始文本，修复因 save_context 中断导致的末尾逗号截断
          std::string raw_json((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
          size_t last_char = raw_json.find_last_not_of(" \t\n\r");
          if (last_char != std::string::npos && raw_json[last_char] == ',') {
            RCLCPP_WARN(get_logger(), "JSON 末尾为逗号（截断），自动修正为 ]");
            raw_json[last_char] = ']';
          }
          auto j = nlohmann::json::parse(raw_json);
          if (j.is_array()) {
            messages_ = j.get<std::vector<nlohmann::json>>();
            if (recover_incomplete_tool_call()) {
              RCLCPP_INFO(get_logger(), cloud_soul::Msg::RECOVERED_TOOL_CALL);
            }
            RCLCPP_INFO(get_logger(), "从文件恢复 %zu 条消息", messages_.size());
            return;
          }
        } catch (...) {
          RCLCPP_WARN(get_logger(), cloud_soul::Msg::CONTEXT_PARSE_FAILED);
        }
      }
    }

    std::string rule = load_rule_text();
    messages_.clear();
    messages_.push_back({{"role", "system"}, {"content", rule}});
    save_context();
  }

  // ---------------------------------------------------------------
  // 工具执行（带安全 JSON 解析）
  // ---------------------------------------------------------------
  nlohmann::json execute_tool(const std::string& name, const std::string& arguments_json) {
    // 预解析参数（只做一次）
    nlohmann::json args;
    try {
      args = nlohmann::json::parse(arguments_json);
    } catch (const std::exception& e) {
      auto repaired = repair_json_args(arguments_json);
      try {
        args = nlohmann::json::parse(repaired);
      } catch (const std::exception& e2) {
        RCLCPP_WARN(get_logger(), "工具参数解析失败: %s", e2.what());
        return {{"error", std::string(cloud_soul::Msg::TOOL_PARAM_INVALID) + e2.what()}};
      }
    }

    auto goal = ExecuteTool::Goal();
    nlohmann::json call_obj = {{"name", name}, {"arguments", args}};
    goal.input_json = call_obj.dump();

    // 带重试的工具调用循环
    for (int attempt = 1; attempt <= cloud_soul::TOOL_RETRY_MAX; ++attempt) {
      if (!action_client_->wait_for_action_server(2s)) {
        if (attempt < cloud_soul::TOOL_RETRY_MAX) {
          RCLCPP_WARN(get_logger(), cloud_soul::Msg::TOOL_RETRY_MSG, attempt, cloud_soul::TOOL_RETRY_MAX);
          std::this_thread::sleep_for(cloud_soul::TOOL_RETRY_INTERVAL);
          continue;
        }
        return {{"error", cloud_soul::Msg::ACTION_SRV_NOT_READY}};
      }

      auto send_future = action_client_->async_send_goal(goal);
      if (send_future.wait_for(cloud_soul::ACTION_REQUEST_TIMEOUT) != std::future_status::ready) {
        if (attempt < cloud_soul::TOOL_RETRY_MAX) {
          RCLCPP_WARN(get_logger(), cloud_soul::Msg::TOOL_RETRY_MSG, attempt, cloud_soul::TOOL_RETRY_MAX);
          std::this_thread::sleep_for(cloud_soul::TOOL_RETRY_INTERVAL);
          continue;
        }
        return {{"error", cloud_soul::Msg::TOOL_CALL_TIMEOUT}};
      }
      auto goal_handle = send_future.get();
      if (!goal_handle) {
        if (attempt < cloud_soul::TOOL_RETRY_MAX) {
          RCLCPP_WARN(get_logger(), cloud_soul::Msg::TOOL_RETRY_MSG, attempt, cloud_soul::TOOL_RETRY_MAX);
          std::this_thread::sleep_for(cloud_soul::TOOL_RETRY_INTERVAL);
          continue;
        }
        return {{"error", cloud_soul::Msg::TOOL_GOAL_REJECTED}};
      }
      auto result_future = action_client_->async_get_result(goal_handle);
      if (result_future.wait_for(tool_timeout_duration_) != std::future_status::ready) {
        if (attempt < cloud_soul::TOOL_RETRY_MAX) {
          RCLCPP_WARN(get_logger(), cloud_soul::Msg::TOOL_RETRY_MSG, attempt, cloud_soul::TOOL_RETRY_MAX);
          std::this_thread::sleep_for(cloud_soul::TOOL_RETRY_INTERVAL);
          continue;
        }
        return {{"error", cloud_soul::Msg::TOOL_EXEC_TIMEOUT}};
      }
      auto result = result_future.get();
      if (result.code != rclcpp_action::ResultCode::SUCCEEDED) {
        if (attempt < cloud_soul::TOOL_RETRY_MAX) {
          RCLCPP_WARN(get_logger(), cloud_soul::Msg::TOOL_RETRY_MSG, attempt, cloud_soul::TOOL_RETRY_MAX);
          std::this_thread::sleep_for(cloud_soul::TOOL_RETRY_INTERVAL);
          continue;
        }
        return {{"error", cloud_soul::Msg::TOOL_EXEC_FAILED}};
      }

      std::string raw = result.result->output_json;
      std::string safe = sanitize_json_string(raw);
      try {
        return nlohmann::json::parse(safe);
      } catch (...) {
        return {{"error", cloud_soul::Msg::TOOL_OUTPUT_INVALID}};
      }
    }

    return {{"error", cloud_soul::Msg::TOOL_EXEC_FAILED}};
  }

  // ---------------------------------------------------------------
  // LLM 调用
  // ---------------------------------------------------------------
  nlohmann::json call_llm(openai_client::OpenAIClient& client, const nlohmann::json& tools) {
    while (rclcpp::ok()) {
      try {
        return client.call_api(false, tools);
      } catch (const std::exception& e) {
        RCLCPP_WARN(get_logger(), "LLM 调用失败: %s, 重试...", e.what());
        std::this_thread::sleep_for(2s);
      }
    }
    return {};
  }

  // ---------------------------------------------------------------
  // 刷新 LLM 客户端
  // ---------------------------------------------------------------
  void refresh_client(openai_client::OpenAIClient& client) {
    client.clear_messages();
    for (const auto& m : messages_) {
    // 跳过空对象 {}（save_context 序列化失败时写入的占位符），避免发送 role=null
    if (m.empty() || !m.contains("role")) continue;
      auto clean = m;
      if (clean.value("role", "") == "assistant") {
        bool has_tc = clean.contains("tool_calls") && !clean["tool_calls"].is_null() &&
                      clean["tool_calls"].is_array() && !clean["tool_calls"].empty();
        if (has_tc) {
          clean["content"] = nullptr;
        }
        // clean.erase("reasoning_content");
      }
      // 确保 tool 消息的 content 是字符串（兼容历史）
      if (clean.value("role", "") == "tool" && clean.contains("content")) {
        if (!clean["content"].is_string()) {
          clean["content"] = clean["content"].dump();
        }
      }
      sanitize_json_utf8(clean);
      client.add_message(clean);
    }
  }

  // ---------------------------------------------------------------
  // 单步思考
  // ---------------------------------------------------------------
  void single_step(openai_client::OpenAIClient& client, const nlohmann::json& tools) {
    auto reply = call_llm(client, tools);
    sanitize_json_recursive(reply);

    nlohmann::json assistant_msg = {{"role", "assistant"}};
    bool has_tool_calls = reply.contains("tool_calls") && !reply["tool_calls"].is_null() &&
                          reply["tool_calls"].is_array() && !reply["tool_calls"].empty();

    if (has_tool_calls) {
      assistant_msg["content"] = nullptr;
      assistant_msg["tool_calls"] = reply["tool_calls"];
    } else {
      assistant_msg["content"] = reply.value("content", "");
    }

    if (reply.contains("reasoning_content") && !reply["reasoning_content"].is_null()) {
        assistant_msg["reasoning_content"] = reply["reasoning_content"];
    }

    messages_.push_back(assistant_msg);
    save_context();

    if (!has_tool_calls) {
      messages_.push_back({{"role", "user"}, {"content", cloud_soul::Msg::NO_CHANNEL_REMINDER}});
      save_context();
      refresh_client(client);
      return;
    }

    refresh_client(client);

    for (const auto& tc : reply["tool_calls"]) {
      std::string func = tc["function"]["name"];
      std::string args = tc["function"]["arguments"];
      std::string id   = tc.value("id", "");

      nlohmann::json result = execute_tool(func, args);
      sanitize_json_recursive(result);
      nlohmann::json tool_msg = {
        {"role", "tool"},
        {"tool_call_id", id},
        {"content", result.dump()}
      };
      messages_.push_back(tool_msg);
      save_context();

      if (func == "user_notify") {
        try {
          auto a = nlohmann::json::parse(args);
          if (a.contains("message") && !a["message"].is_null()) {
            auto pub_msg = std_msgs::msg::String();
            pub_msg.data = a["message"].get<std::string>();
            response_pub_->publish(pub_msg);
          }
        } catch (...) {}
      }
    }
  }

  // ---------------------------------------------------------------
  // 对话轮次切分
  // ---------------------------------------------------------------
  static std::vector<std::vector<nlohmann::json>> split_into_turns(const std::vector<nlohmann::json>& msgs) {
    std::vector<std::vector<nlohmann::json>> turns;
    std::vector<nlohmann::json> current;
    for (const auto& m : msgs) {
      if (m.value("role", "") == "user") {
        if (!current.empty()) turns.push_back(std::move(current));
        current.clear();
      }
      current.push_back(m);
    }
    if (!current.empty()) turns.push_back(std::move(current));
    return turns;
  }

  // ---------------------------------------------------------------
  // 上下文压缩
  // ---------------------------------------------------------------
  void compress_context() {
    RCLCPP_INFO(get_logger(), "开始上下文压缩...");
    save_context();

    while (rclcpp::ok()) {
      auto req = std::make_shared<MemoryArchive::Request>();
      req->json_path = context_file_path_;
      auto future = archive_client_->async_send_request(req);
      if (future.wait_for(cloud_soul::COMPRESS_SERVICE_TIMEOUT) == std::future_status::ready) {
        auto resp = future.get();
        if (resp && resp->error_code == 0) break;
      }
      std::this_thread::sleep_for(2s);
    }

    auto turns = split_into_turns(messages_);
    size_t keep = std::min(turns.size(), static_cast<size_t>(summary_turns_));
    std::vector<nlohmann::json> recent;
    for (size_t i = turns.size() - keep; i < turns.size(); ++i)
      recent.insert(recent.end(), turns[i].begin(), turns[i].end());

    constexpr size_t LOCAL_MAX_TOOL_CONTENT_LEN = cloud_soul::MAX_TOOL_CONTENT_LEN;
    for (auto& msg : recent) {
      if (msg.value("role", "") == "tool" && msg.contains("content")) {
        std::string content = msg["content"].get<std::string>();
        if (content.size() > LOCAL_MAX_TOOL_CONTENT_LEN) {
          content = content.substr(0, LOCAL_MAX_TOOL_CONTENT_LEN) + "\n...[工具输出已截断]";
          msg["content"] = content;
        }
      }
    }

    std::string summary;
    for (size_t i = 0; i < keep; ++i) {
      const auto& turn = turns[turns.size() - keep + i];
      for (const auto& m : turn) {
        if (m.value("role", "") == "user")
          summary += "用户: " + m.value("content", "") + "\n";
        else if (m.value("role", "") == "assistant" && m.contains("content") && m["content"].is_string() && !m["content"].get<std::string>().empty())
          summary += "助手: " + m["content"].get<std::string>() + "\n";
      }
      summary += "\n";
    }

    std::string rule = load_rule_text();
    messages_.clear();
    messages_.push_back({{"role", "system"}, {"content", rule + "\n\n[对话摘要]\n" + summary}});
    for (auto& m : recent) {
      if (m.value("role", "") == "system") continue;
      messages_.push_back(std::move(m));
    }
    messages_.push_back({{"role", "user"}, {"content", cloud_soul::Msg::COMPRESSION_REMINDER}});

    context_file_path_ = create_new_context_filename();
    save_context();
    RCLCPP_INFO(get_logger(), "压缩完成，新文件: %s", context_file_path_.c_str());
  }

  // ---------------------------------------------------------------
  // 主循环
  // ---------------------------------------------------------------
  void run_loop() {
    wait_for_service<GetSnapshot>(input_client_, "input");
    wait_for_service<GetToolsInfo>(tools_client_, "output/info");
    wait_for_service<MemoryRecall>(recall_client_, "memory_recall");
    wait_for_service<MemoryArchive>(archive_client_, "memory_archive");
    if (!action_client_->wait_for_action_server(2s)) {
      RCLCPP_WARN(get_logger(), cloud_soul::Msg::OUTPUT_ACTION_NOT_RDY);
    }

    initialize_messages();

    // ---- 零号思考 ----
    nlohmann::json tools = nlohmann::json::array();
    openai_client::OpenAIClient client(openai_base_url_, openai_api_key_, openai_model_);
    client.set_thinking_enabled(true);
    refresh_client(client);
    single_step(client, tools);

    std::string last_hash;
    while (rclcpp::ok()) {
      // ---- 1) 获取输入快照 ----
      auto input_resp = call_service_reliably<GetSnapshot>(input_client_, "input");
      if (!input_resp) continue;

      std::string snapshot = input_resp->snapshot_json;
      if (!snapshot.empty()) {
        std::string h = std::to_string(std::hash<std::string>{}(snapshot));
        if (h == last_hash) {
          if (loop_rate_ <= 0.0) {
            std::this_thread::sleep_for(cloud_soul::SERVICE_RETRY_INTERVAL);
          }
          continue;
        }
        last_hash = h;
        messages_.push_back({{"role", "user"}, {"content", snapshot}});
        save_context();
      } else {
        if (loop_rate_ <= 0.0) {
          std::this_thread::sleep_for(cloud_soul::SERVICE_RETRY_INTERVAL);
          continue;
        } else {
          std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / loop_rate_));
          continue;
        }
      }

      // ---- 2) 获取工具列表 ----
      auto tools_resp = call_service_reliably<GetToolsInfo>(tools_client_, "output/info");
      tools = nlohmann::json::array();
      if (tools_resp) {
        try {
          tools = nlohmann::json::parse(tools_resp->tools_json);
          if (!tools.is_array()) tools = nlohmann::json::array();
        } catch (...) {}
      }

      // ---- 3) 刷新客户端并执行单步思考 ----
      refresh_client(client);
      single_step(client, tools);

      // ---- 4) 压缩检查 ----
      if (estimate_tokens(messages_) >= max_context_tokens_) {
        compress_context();
      }

      // ---- 5) 频率控制 ----
      if (loop_rate_ > 0.0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / loop_rate_));
      }
    }
  }

  // ---------------------------------------------------------------
  // 成员变量
  // ---------------------------------------------------------------
  std::string agent_name_;
  std::string context_dir_;
  std::string context_file_path_;
  int max_context_tokens_;
  int summary_turns_;
  double loop_rate_;
  std::chrono::duration<double> tool_timeout_duration_;
  std::string openai_base_url_;
  std::string openai_api_key_;
  std::string openai_model_;

  rclcpp::Client<GetSnapshot>::SharedPtr   input_client_;
  rclcpp::Client<GetToolsInfo>::SharedPtr  tools_client_;
  rclcpp::Client<MemoryRecall>::SharedPtr  recall_client_;
  rclcpp::Client<MemoryArchive>::SharedPtr archive_client_;
  rclcpp_action::Client<ExecuteTool>::SharedPtr action_client_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr response_pub_;

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