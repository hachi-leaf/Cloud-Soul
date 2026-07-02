// Copyright (c) leaf
// SPDX-License-Identifier: MIT

#include "cs_core/call_openai.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>
#include <cstring>
#include <cstdio>

namespace openai_client {

struct OpenAIClient::Impl {
    // ---------- 不可变成员 ----------
    std::string url_;
    std::string api_key_;

    // ---------- 可调参数 ----------
    std::string model_;
    double temperature_ = 1.0;
    double top_p_ = 1.0;
    int max_tokens_ = 32768;
    bool thinking_enabled_ = false;
    std::string reasoning_effort_ = "high";
    std::string response_format_type_ = "text";
    std::vector<std::string> stop_;
    bool logprobs_ = false;
    std::string tool_choice_ = "auto";
    double timeout_sec_ = 0.0;  // 0 = no timeout

    // ---------- 消息历史 ----------
    std::vector<nlohmann::json> messages_;

    // ---------- 回调 ----------
    ThinkCallback think_cb_;
    ReplyCallback reply_cb_;

    // ---------- 同步原语 ----------
    mutable std::mutex data_mutex_;
    std::mutex request_mutex_;
    std::atomic<bool> cancel_flag_{false};

    // ---------- curl 全局初始化 ----------
    static std::once_flag curl_init_flag_;
    static void init_curl() { curl_global_init(CURL_GLOBAL_ALL); }

    Impl(const std::string& url, const std::string& api_key, const std::string& model)
        : url_(url), api_key_(api_key), model_(model)
    {
        while (!url_.empty() && url_.back() == '/')
            url_.pop_back();
        std::call_once(curl_init_flag_, init_curl);
    }
};

// ---------- 构造函数 ----------
OpenAIClient::OpenAIClient(const std::string& url,
                           const std::string& api_key,
                           const std::string& model)
    : impl_(std::make_unique<Impl>(url, api_key, model))
{}

OpenAIClient::~OpenAIClient() = default;

OpenAIClient::OpenAIClient(OpenAIClient&&) noexcept = default;
OpenAIClient& OpenAIClient::operator=(OpenAIClient&&) noexcept = default;

// ---------- 参数 getter/setter ----------
void OpenAIClient::set_model(const std::string& model) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->model_ = model;
}
std::string OpenAIClient::get_model() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->model_;
}

void OpenAIClient::set_temperature(double temp) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->temperature_ = temp;
}
double OpenAIClient::get_temperature() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->temperature_;
}

void OpenAIClient::set_top_p(double top_p) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->top_p_ = top_p;
}
double OpenAIClient::get_top_p() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->top_p_;
}

void OpenAIClient::set_max_tokens(int max_tokens) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->max_tokens_ = max_tokens;
}
int OpenAIClient::get_max_tokens() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->max_tokens_;
}

void OpenAIClient::set_thinking_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->thinking_enabled_ = enabled;
}
bool OpenAIClient::get_thinking_enabled() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->thinking_enabled_;
}

void OpenAIClient::set_reasoning_effort(const std::string& effort) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->reasoning_effort_ = effort;
}
std::string OpenAIClient::get_reasoning_effort() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->reasoning_effort_;
}

void OpenAIClient::set_response_format_type(const std::string& type) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->response_format_type_ = type;
}
std::string OpenAIClient::get_response_format_type() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->response_format_type_;
}

void OpenAIClient::set_stop(const std::vector<std::string>& stop) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->stop_ = stop;
}
std::vector<std::string> OpenAIClient::get_stop() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->stop_;
}

void OpenAIClient::set_logprobs(bool enable) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->logprobs_ = enable;
}
bool OpenAIClient::get_logprobs() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->logprobs_;
}

void OpenAIClient::set_tool_choice(const std::string& choice) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->tool_choice_ = choice;
}
std::string OpenAIClient::get_tool_choice() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->tool_choice_;
}

void OpenAIClient::set_timeout(double timeout_sec) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->timeout_sec_ = timeout_sec;
}
double OpenAIClient::get_timeout() const {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    return impl_->timeout_sec_;
}

// ---------- 消息管理 ----------
void OpenAIClient::add_message(const nlohmann::json& message) {
    if (!message.is_object())
        throw std::invalid_argument("add_message: message must be a JSON object");
    if (!message.contains("role"))
        throw std::invalid_argument("add_message: message must contain 'role'");
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->messages_.push_back(message);
}

void OpenAIClient::clear_messages() {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->messages_.clear();
}

// ---------- 回调 ----------
void OpenAIClient::set_think_callback(ThinkCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->think_cb_ = std::move(cb);
}
void OpenAIClient::set_reply_callback(ReplyCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->data_mutex_);
    impl_->reply_cb_ = std::move(cb);
}

void OpenAIClient::cancel_request() {
    impl_->cancel_flag_.store(true, std::memory_order_release);
}

// ---------- 流式上下文 ----------
struct StreamContext {
    std::string* content;
    std::string* reasoning;
    std::map<int, nlohmann::json>* tool_calls;
    OpenAIClient::ThinkCallback think_cb;
    OpenAIClient::ReplyCallback reply_cb;
    std::atomic<bool>* cancel_flag;
    std::string line_buf;
    int* out_input_tokens = nullptr;
};

// ---------- CURL 写回调 ----------
static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamContext*>(userdata);
    if (ctx->cancel_flag->load(std::memory_order_acquire))
        return 0;

    size_t total = size * nmemb;
    ctx->line_buf.append(ptr, total);

    while (true) {
        auto pos = ctx->line_buf.find('\n');
        if (pos == std::string::npos) break;
        std::string line = ctx->line_buf.substr(0, pos);
        ctx->line_buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;

        if (line.rfind("data: ", 0) != 0) continue;
        std::string data = line.substr(6);
        if (data == "[DONE]") continue;

        try {
            auto j = nlohmann::json::parse(data);

            if (j.contains("usage") && j["usage"].is_object()) {
                if (ctx->out_input_tokens) {
                    int prompt_tokens = j["usage"].value("prompt_tokens", -1);
                    if (prompt_tokens >= 0) {
                        *ctx->out_input_tokens = prompt_tokens;
                    }
                }
            }

            if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
                continue;
            const auto& choice = j["choices"][0];
            if (!choice.contains("delta") || !choice["delta"].is_object())
                continue;
            const auto& delta = choice["delta"];

            if (delta.contains("content") && delta["content"].is_string()) {
                std::string chunk = delta["content"].get<std::string>();
                ctx->content->append(chunk);
                if (ctx->reply_cb) ctx->reply_cb(chunk);
            }

            if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                std::string chunk = delta["reasoning_content"].get<std::string>();
                ctx->reasoning->append(chunk);
                if (ctx->think_cb) ctx->think_cb(chunk);
            }

            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (const auto& tc : delta["tool_calls"]) {
                    int idx = tc.value("index", 0);
                    auto it = ctx->tool_calls->find(idx);
                    if (it == ctx->tool_calls->end()) {
                        nlohmann::json call;
                        call["id"] = tc.value("id", "");
                        call["type"] = "function";
                        call["function"] = {{"name", ""}, {"arguments", ""}};
                        it = ctx->tool_calls->emplace(idx, std::move(call)).first;
                    }
                    if (tc.contains("id") && !tc["id"].is_null())
                        it->second["id"] = tc["id"];
                    if (tc.contains("type") && !tc["type"].is_null())
                        it->second["type"] = tc["type"];
                    if (tc.contains("function")) {
                        const auto& func = tc["function"];
                        if (func.contains("name") && !func["name"].is_null())
                            it->second["function"]["name"] = func["name"];
                        if (func.contains("arguments") && func["arguments"].is_string())
                            it->second["function"]["arguments"] =
                                it->second["function"]["arguments"].get<std::string>() +
                                func["arguments"].get<std::string>();
                    }
                }
            }
        } catch (...) {
        }
    }
    return total;
}

// ---------- CURL 进度回调 ----------
static int progress_callback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                             curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* flag = static_cast<std::atomic<bool>*>(clientp);
    if (flag->load(std::memory_order_acquire)) return 1;
    return 0;
}

// ---------- call_api 实现 ----------
nlohmann::json OpenAIClient::call_api(bool stream,
                                     const nlohmann::json& tools,
                                     int* input_tokens)
{
    if (!tools.is_null() && !tools.is_array()) {
        throw std::invalid_argument("call_api: tools must be a JSON array or null");
    }

    // 1. 锁定数据并拷贝当前配置与消息历史
    std::string model;
    double temperature, top_p;
    int max_tokens;
    bool thinking_enabled, logprobs;
    std::string reasoning_effort, response_format_type, tool_choice;
    std::vector<std::string> stop;
    std::vector<nlohmann::json> messages;
    ThinkCallback think_cb;
    ReplyCallback reply_cb;

    {
        std::lock_guard<std::mutex> lock(impl_->data_mutex_);
        model = impl_->model_;
        temperature = impl_->temperature_;
        top_p = impl_->top_p_;
        max_tokens = impl_->max_tokens_;
        thinking_enabled = impl_->thinking_enabled_;
        reasoning_effort = impl_->reasoning_effort_;
        response_format_type = impl_->response_format_type_;
        stop = impl_->stop_;
        logprobs = impl_->logprobs_;
        tool_choice = impl_->tool_choice_;
        messages = impl_->messages_;
        think_cb = impl_->think_cb_;
        reply_cb = impl_->reply_cb_;
    }

    // 2. 构建请求体（仅包含必要字段，适配 DeepSeek 文档）
    nlohmann::json body;
    body["model"] = model;
    body["messages"] = messages;
    body["stream"] = stream;
    if (temperature != 1.0) body["temperature"] = temperature;
    if (top_p != 1.0) body["top_p"] = top_p;
    body["max_tokens"] = max_tokens;
    // 不发送 logprobs（DeepSeek 文档未列出）
    if (response_format_type == "json_object") {
        body["response_format"] = {{"type", "json_object"}};
    }
    if (!stop.empty()) body["stop"] = stop;
    if (!tools.is_null() && !tools.empty()) {
        body["tools"] = tools;
        body["tool_choice"] = tool_choice;
    }
    if (thinking_enabled) {
        nlohmann::json thinking_obj;
        thinking_obj["type"] = "enabled";
        thinking_obj["reasoning_effort"] = reasoning_effort;
        body["thinking"] = thinking_obj;
    }

    std::string body_str = body.dump();

    // 3. 初始化 curl
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to create curl handle");

    std::string full_url = impl_->url_ + "/chat/completions";
    std::string response_body;
    StreamContext stream_ctx;
    std::string accumulated_content;
    std::string accumulated_reasoning;
    std::map<int, nlohmann::json> tool_calls_map;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + impl_->api_key_).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    if (impl_->timeout_sec_ > 0.0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(impl_->timeout_sec_ * 1000.0));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (input_tokens) {
        *input_tokens = -1;
    }

    if (stream) {
        stream_ctx.content = &accumulated_content;
        stream_ctx.reasoning = &accumulated_reasoning;
        stream_ctx.tool_calls = &tool_calls_map;
        stream_ctx.think_cb = std::move(think_cb);
        stream_ctx.reply_cb = std::move(reply_cb);
        stream_ctx.cancel_flag = &impl_->cancel_flag_;
        stream_ctx.line_buf.clear();
        stream_ctx.out_input_tokens = input_tokens;

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream_ctx);

        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &impl_->cancel_flag_);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t nmemb, void* userp) {
            auto* str = static_cast<std::string*>(userp);
            str->append(data, size * nmemb);
            return size * nmemb;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    }

    // 4. 执行请求
    {
        std::lock_guard<std::mutex> req_lock(impl_->request_mutex_);
        impl_->cancel_flag_.store(false, std::memory_order_release);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            bool cancelled = impl_->cancel_flag_.load(std::memory_order_acquire);
            if (stream && cancelled && !accumulated_content.empty()) {
                goto build_stream_response;
            }
            throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));
        }
    }

    // 5. 处理响应
    if (!stream) {
        // 调试：打印 API 原始响应
        fprintf(stderr, "DEBUG API response body: %s\n", response_body.c_str());

        auto j = nlohmann::json::parse(response_body);

        // 检查 API 是否返回了错误
        if (j.contains("error")) {
            std::string err_msg = j["error"].value("message", "unknown error");
            std::string err_type = j["error"].value("type", "unknown");
            throw std::runtime_error("API returned error: [" + err_type + "] " + err_msg);
        }

        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
            throw std::runtime_error("Invalid API response: missing choices, body: " + response_body.substr(0, 200));

        // 提取输入 token 数
        if (input_tokens && j.contains("usage") && j["usage"].is_object()) {
            int prompt_tokens = j["usage"].value("prompt_tokens", -1);
            if (prompt_tokens >= 0) {
                *input_tokens = prompt_tokens;
            }
        }

        return j["choices"][0].value("message", nlohmann::json::object());
    }

build_stream_response:
    // 流式：构造消息对象
    nlohmann::json msg;
    msg["role"] = "assistant";
    if (!accumulated_reasoning.empty())
        msg["reasoning_content"] = accumulated_reasoning;

    if (!tool_calls_map.empty()) {
        std::vector<nlohmann::json> tool_calls_array;
        for (const auto& [idx, call] : tool_calls_map)
            tool_calls_array.push_back(call);
        msg["tool_calls"] = tool_calls_array;
    } else {
        msg["content"] = accumulated_content;
    }

    return msg;
}

std::once_flag OpenAIClient::Impl::curl_init_flag_;

} // namespace openai_client