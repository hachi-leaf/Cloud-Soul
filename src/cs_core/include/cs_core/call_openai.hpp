// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// OpenAI 兼容的聊天补全客户端，支持流式、思考过程回调、工具调用、中途取消。
//
// 用法:
//   openai_client::OpenAIClient client("https://api.openai.com/v1", "sk-xxxx", "gpt-4o");
//   client.set_temperature(0.7);
//   client.add_message({{"role","system"}, {"content","You are helpful"}});
//   client.add_message({{"role","user"}, {"content","Hi"}});
//   client.set_think_callback([](const std::string& t){ std::cout << "[think] " << t; });
//   // 非流式调用，返回 assistant 消息对象
//   nlohmann::json reply = client.call_api(false);
//   // 流式 + 工具调用
//   nlohmann::json tools = nlohmann::json::parse(R"([{"type":"function",...}])");
//   nlohmann::json reply2 = client.call_api(true, tools);
//   // 中途失败（取消或网络错误）会返回包含已接收部分内容的 assistant 消息。
#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace openai_client {

class OpenAIClient {
public:
    /// @param url      API 地址 (如 "https://api.openai.com/v1")，尾部斜杠可选
    /// @param api_key  认证密钥
    /// @param model    默认模型名，可后续修改
    OpenAIClient(const std::string& url,
                 const std::string& api_key,
                 const std::string& model = "gpt-3.5-turbo");
    ~OpenAIClient();

    // 不可拷贝
    OpenAIClient(const OpenAIClient&) = delete;
    OpenAIClient& operator=(const OpenAIClient&) = delete;
    // 可移动
    OpenAIClient(OpenAIClient&&) noexcept;
    OpenAIClient& operator=(OpenAIClient&&) noexcept;

    // ---------- 参数设置/获取 ----------
    void set_model(const std::string& model);
    std::string get_model() const;

    void set_temperature(double temp);
    double get_temperature() const;

    void set_top_p(double top_p);
    double get_top_p() const;

    void set_max_tokens(int max_tokens);
    int get_max_tokens() const;

    // 启用/禁用思考输出 (对应 thinking.type = "enabled"/"disabled")
    void set_thinking_enabled(bool enabled);
    bool get_thinking_enabled() const;

    // 推理强度 "high"/"medium"/"low"
    void set_reasoning_effort(const std::string& effort);
    std::string get_reasoning_effort() const;

    // response_format 的 type 字段，如 "text" / "json_object"
    void set_response_format_type(const std::string& type);
    std::string get_response_format_type() const;

    void set_stop(const std::vector<std::string>& stop);
    std::vector<std::string> get_stop() const;

    void set_logprobs(bool enable);
    bool get_logprobs() const;

    void set_tool_choice(const std::string& tool_choice);
    std::string get_tool_choice() const;

    // ---------- 消息管理 ----------
    /// 将一条消息追加到内部历史。消息必须为对象，且至少包含 "role" 字段
    void add_message(const nlohmann::json& message);
    void clear_messages();

    // ---------- 流式回调 ----------
    using ThinkCallback = std::function<void(const std::string&)>; ///< 思考过程增量
    using ReplyCallback = std::function<void(const std::string&)>; ///< 回复内容增量

    void set_think_callback(ThinkCallback cb);
    void set_reply_callback(ReplyCallback cb);

    // ---------- 核心调用 ----------
    /// 使用内部消息列表发起请求
    /// @param stream 是否流式
    /// @param tools  工具定义数组 (JSON 数组)，传 nullptr 表示不用工具
    /// @return 助手消息对象，格式与非流式一致。流式中途失败/取消时返回已收到的部分内容
    nlohmann::json call_api(bool stream = false,
                            const nlohmann::json& tools = nullptr);

    /// 取消正在进行的流式请求（非流式调用无法中途取消）
    void cancel_request();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openai_client