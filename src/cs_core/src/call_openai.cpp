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

namespace openai_client {

struct OpenAIClient::Impl {
    // ---------- 不可变成员 ----------
    std::string url_;
    std::string api_key_;

    // ---------- 可调参数 ----------
    std::string model_;
    double temperature_ = 1.0;
    double top_p_ = 1.0;
    int max_tokens_ = 4096;
    bool thinking_enabled_ = false;
    std::string reasoning_effort_ = "high";
    std::string response_format_type_ = "text";
    std::vector<std::string> stop_;
    bool logprobs_ = false;
    std::string tool_choice_ = "auto";

    // ---------- 消息历史 ----------
    std::vector<nlohmann::json> messages_;

    // ---------- 回调 ----------
    ThinkCallback think_cb_;
    ReplyCallback reply_cb_;

    // ---------- 同步原语 ----------
    mutable std::mutex data_mutex_;   // 保护参数和消息
    std::mutex request_mutex_;        // 保证同一时间只有一个网络请求
    std::atomic<bool> cancel_flag_{false};

    // ---------- curl 全局初始化 ----------
    static std::once_flag curl_init_flag_;
    static void init_curl() { curl_global_init(CURL_GLOBAL_ALL); }

    Impl(const std::string& url, const std::string& api_key, const std::string& model)
        : url_(url), api_key_(api_key), model_(model)
    {
        // 去掉尾部斜杠，后续统一加 "/chat/completions"
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

// ---------- 流式上下文（栈对象） ----------
struct StreamContext {
    std::string* content;
    std::string* reasoning;
    std::map<int, nlohmann::json>* tool_calls;
    OpenAIClient::ThinkCallback think_cb;
    OpenAIClient::ReplyCallback reply_cb;
    std::atomic<bool>* cancel_flag;
    std::string line_buf;
};

// ---------- CURL 写回调：处理 SSE ----------
static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamContext*>(userdata);
    if (ctx->cancel_flag->load(std::memory_order_acquire))
        return 0; // 返回0使 curl 上报写入错误

    size_t total = size * nmemb;
    ctx->line_buf.append(ptr, total);

    // 按行解析
    while (true) {
        auto pos = ctx->line_buf.find('\n');
        if (pos == std::string::npos) break;
        std::string line = ctx->line_buf.substr(0, pos);
        ctx->line_buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;

        if (line.rfind("data: ", 0) != 0) continue; // 只关心 data 行
        std::string data = line.substr(6);
        if (data == "[DONE]") continue;

        try {
            auto j = nlohmann::json::parse(data);
            if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
                continue;
            const auto& choice = j["choices"][0];
            if (!choice.contains("delta") || !choice["delta"].is_object())
                continue;
            const auto& delta = choice["delta"];

            // 普通文本内容
            if (delta.contains("content") && delta["content"].is_string()) {
                std::string chunk = delta["content"].get<std::string>();
                ctx->content->append(chunk);
                if (ctx->reply_cb) ctx->reply_cb(chunk);
            }

            // 思考内容（DeepSeek 等）
            if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                std::string chunk = delta["reasoning_content"].get<std::string>();
                ctx->reasoning->append(chunk);
                if (ctx->think_cb) ctx->think_cb(chunk);
            }

            // 工具调用
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (const auto& tc : delta["tool_calls"]) {
                    int idx = tc.value("index", 0);
                    auto it = ctx->tool_calls->find(idx);
                    if (it == ctx->tool_calls->end()) {
                        // 新建工具调用占位
                        nlohmann::json call;
                        call["id"] = tc.value("id", "");
                        call["type"] = "function";
                        call["function"] = {{"name", ""}, {"arguments", ""}};
                        it = ctx->tool_calls->emplace(idx, std::move(call)).first;
                    }
                    // 合并字段
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
            // 忽略解析异常，继续处理后续数据
        }
    }
    return total;
}

// ---------- CURL 进度回调：允许快速取消 ----------
static int progress_callback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                             curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* flag = static_cast<std::atomic<bool>*>(clientp);
    if (flag->load(std::memory_order_acquire)) return 1; // 返回非0使 curl 中止
    return 0;
}

// ---------- call_api 实现 ----------
nlohmann::json OpenAIClient::call_api(bool stream, const nlohmann::json& tools) {
    // 校验 tools 参数
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

    // 2. 构建请求体
    nlohmann::json body;
    body["model"] = model;
    body["messages"] = messages;
    body["stream"] = stream;
    body["temperature"] = temperature;
    body["top_p"] = top_p;
    body["max_tokens"] = max_tokens;
    body["logprobs"] = logprobs;
    body["response_format"] = {{"type", response_format_type}};
    if (!stop.empty()) body["stop"] = stop;
    if (!tools.is_null()) {
        body["tools"] = tools;
        body["tool_choice"] = tool_choice;
    }
    if (thinking_enabled) {
        body["thinking"] = {{"type", "enabled"}};
        body["reasoning_effort"] = reasoning_effort;
    }

    std::string body_str = body.dump();

    // 3. 初始化 curl（每次调用创建 easy handle，线程安全）
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
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (stream) {
        // 流式配置
        stream_ctx.content = &accumulated_content;
        stream_ctx.reasoning = &accumulated_reasoning;
        stream_ctx.tool_calls = &tool_calls_map;
        stream_ctx.think_cb = std::move(think_cb);
        stream_ctx.reply_cb = std::move(reply_cb);
        stream_ctx.cancel_flag = &impl_->cancel_flag_;
        stream_ctx.line_buf.clear();

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream_ctx);

        // 进度回调用于及时取消
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &impl_->cancel_flag_);
    } else {
        // 非流式：写入字符串
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t nmemb, void* userp) {
            auto* str = static_cast<std::string*>(userp);
            str->append(data, size * nmemb);
            return size * nmemb;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    }

    // 4. 执行请求（串行化网络调用）
    {
        std::lock_guard<std::mutex> req_lock(impl_->request_mutex_);
        // 重置取消标志
        impl_->cancel_flag_.store(false, std::memory_order_release);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            // 检查是否为取消
            bool cancelled = impl_->cancel_flag_.load(std::memory_order_acquire);
            if (stream && cancelled && !accumulated_content.empty()) {
                goto build_stream_response;
            }
            throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));
        }
    }

    // 5. 处理响应
    if (!stream) {
        // 非流式：解析 JSON 并提取消息
        auto j = nlohmann::json::parse(response_body);
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
            throw std::runtime_error("Invalid API response: missing choices");
        return j["choices"][0].value("message", nlohmann::json::object());
    }

build_stream_response:
    // 流式：构造与非流式一致的消息对象
    nlohmann::json msg;
    msg["role"] = "assistant";
    if (!accumulated_reasoning.empty())
        msg["reasoning_content"] = accumulated_reasoning;

    if (!tool_calls_map.empty()) {
        // 工具调用模式
        std::vector<nlohmann::json> tool_calls_array;
        for (const auto& [idx, call] : tool_calls_map)
            tool_calls_array.push_back(call);
        msg["tool_calls"] = tool_calls_array;
        // 通常不包含 content
    } else {
        msg["content"] = accumulated_content;
    }

    return msg;
}

std::once_flag OpenAIClient::Impl::curl_init_flag_;

} // namespace openai_client