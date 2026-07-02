// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// ================================================================
// Cloud-Soul Web 页面抓取工具节点
// ================================================================
//
// 作用:
//   抓取指定 URL 的网页全文，剥离 HTML 标签/脚本/样式，提取纯文本。
//   线程化执行，支持取消和超时。
//
// 节点名: /<agent_name>/web_fetch_node
//
// 参数:
//   agent_name       (string, 必填)  Agent 命名空间
//   info_rate         (double, 1.0)  发布 Tools Info 的频率（Hz）
//   default_timeout   (double, 30.0)  默认超时秒数
//   max_size_mb       (int, 5)       最大下载文件大小（MB）
//
// Action:
//   /<agent_name>/output/web_fetch  (ExecuteTool)
//     Goal: 接收 {"name":"web_fetch","arguments":{"url":"...","timeout_sec":30}}
//     Result: output_json 为 {"url":...,"size":...,"text":"..."}
//     Cancel: 终止正在进行的 HTTP 请求
//
// 上层传入 JSON 规范 (来自 output_mgmt):
//   output_mgmt 透传以下 JSON 给本节点:
//   {
//     "name": "web_fetch",
//     "arguments": {
//       "url": "https://...",               // 必填，目标 URL
//       "timeout_sec": 30                    // 可选，超时秒数
//     }
//   }
//
// 关键设计:
//   - 线程化执行，支持取消
//   - 全链路无 std::regex（避免栈溢出）
//   - HTML 实体解码 + UTF-8 清洗
//   - 返回文本限制 500KB，保护 LLM 上下文
//

#include <curl/curl.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "cs_interfaces/action/execute_tool.hpp"
#include <std_msgs/msg/string.hpp>

using json = nlohmann::json;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using namespace std::chrono_literals;

// ============================================================
// UTF-8 清洗（移除无效字节序列）
// ============================================================
static std::string sanitize_utf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        unsigned char c = in[i];
        if (c < 0x80) { out += c; i++; }
        else if (c < 0xC0) { out += '?'; i++; }
        else if (c < 0xE0) {
            if (i+1 < in.size() && (in[i+1] & 0xC0) == 0x80) { out += c; out += in[i+1]; }
            else out += '?';
            i += 2;
        } else if (c < 0xF0) {
            if (i+2 < in.size() && (in[i+1] & 0xC0) == 0x80 && (in[i+2] & 0xC0) == 0x80) {
                out += c; out += in[i+1]; out += in[i+2];
            } else out += '?';
            i += 3;
        } else if (c < 0xF8) {
            if (i+3 < in.size() && (in[i+1] & 0xC0) == 0x80 && (in[i+2] & 0xC0) == 0x80 && (in[i+3] & 0xC0) == 0x80) {
                out += c; out += in[i+1]; out += in[i+2]; out += in[i+3];
            } else out += '?';
            i += 4;
        } else { out += '?'; i++; }
    }
    return out;
}

// ============================================================
// HTML 实体解码（去 regex，纯手动扫描）
// ============================================================
static std::string decode_html_entities(const std::string& in) {
    static const std::pair<const char*, const char*> ents[] = {
        {"&ensp;", " "}, {"&emsp;", " "}, {"&nbsp;", " "},
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"}, {"&middot;", "\xc2\xb7"},
        {"&bull;", "\xe2\x80\xa2"}, {"&ndash;", "\xe2\x80\x93"}, {"&mdash;", "\xe2\x80\x94"},
    };
    std::string out; out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '&' || i + 2 >= in.size()) { out += in[i]; i++; continue; }
        bool matched = false;
        for (const auto& [ent, ch] : ents) {
            size_t len = std::strlen(ent);
            if (in.compare(i, len, ent) == 0) {
                out += ch; i += len; matched = true; break;
            }
        }
        if (matched) continue;
        if (in[i+1] == '#' && i + 3 < in.size()) {
            size_t j = i + 2;
            while (j < in.size() && in[j] >= '0' && in[j] <= '9') j++;
            if (j < in.size() && in[j] == ';' && j > i + 2) {
                int code = 0;
                for (size_t k = i + 2; k < j; k++) code = code * 10 + (in[k] - '0');
                if (code < 128) out += (char)code;
                else if (code < 0x800) {
                    out += (char)(0xC0 | (code >> 6));
                    out += (char)(0x80 | (code & 0x3F));
                } else if (code < 0x10000) {
                    out += (char)(0xE0 | (code >> 12));
                    out += (char)(0x80 | ((code >> 6) & 0x3F));
                    out += (char)(0x80 | (code & 0x3F));
                }
                i = j + 1; continue;
            }
        }
        out += in[i]; i++;
    }
    return out;
}

// ============================================================
// 去除 HTML 标签（跳过 <...>）
// ============================================================
static std::string strip_tags(const std::string& in) {
    std::string out; out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '<') { while (i < in.size() && in[i] != '>') i++; continue; }
        out += in[i];
    }
    return out;
}

// ============================================================
// 去除脚本、样式块和 HTML 注释（状态机，无 regex）
// ============================================================
static std::string strip_blocks(const std::string& in) {
    auto icase_find = [&](size_t start, const std::string& needle) -> size_t {
        for (size_t i = start; i + needle.size() <= in.size(); i++) {
            bool ok = true;
            for (size_t j = 0; j < needle.size(); j++) {
                char c = in[i + j], n = needle[j];
                if (c != n && c != (n ^ 0x20)) { ok = false; break; }
            }
            if (ok) return i;
        }
        return std::string::npos;
    };

    std::string out; out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '<') { out += in[i]; i++; continue; }
        if (i + 7 < in.size()) {
            const char* p = in.c_str() + i + 1;
            if ((p[0]=='s'||p[0]=='S') && (p[1]=='c'||p[1]=='C') && (p[2]=='r'||p[2]=='R') &&
                (p[3]=='i'||p[3]=='I') && (p[4]=='p'||p[4]=='P') && (p[5]=='t'||p[5]=='T')) {
                size_t j = i + 7;
                while (j < in.size() && in[j] != '>') j++;
                if (j < in.size()) {
                    j++;
                    size_t close = icase_find(j, "</script>");
                    if (close != std::string::npos) { i = close + 9; continue; }
                }
            }
        }
        if (i + 6 < in.size()) {
            const char* p = in.c_str() + i + 1;
            if ((p[0]=='s'||p[0]=='S') && (p[1]=='t'||p[1]=='T') && (p[2]=='y'||p[2]=='Y') &&
                (p[3]=='l'||p[3]=='L') && (p[4]=='e'||p[4]=='E')) {
                size_t j = i + 6;
                while (j < in.size() && in[j] != '>') j++;
                if (j < in.size()) {
                    j++;
                    size_t close = icase_find(j, "</style>");
                    if (close != std::string::npos) { i = close + 8; continue; }
                }
            }
        }
        if (i + 3 < in.size() && in[i+1]=='!' && in[i+2]=='-' && in[i+3]=='-') {
            size_t close = in.find("-->", i + 4);
            if (close != std::string::npos) { i = close + 3; continue; }
        }
        out += in[i]; i++;
    }
    return out;
}

// ============================================================
// HTML → 纯文本（纯函数，在 worker 线程中调用）
// ============================================================
static json extract_text(const std::string& html, const std::string& final_url,
                         const std::string& orig_url) {
    std::string clean = strip_blocks(html);
    clean = strip_tags(clean);
    clean = decode_html_entities(clean);

    // 压缩空白行：手动折叠
    {
        std::string tmp; tmp.reserve(clean.size());
        int nl = 0;
        for (char c : clean) {
            if (c == '\n') { nl++; }
            else {
                if (nl > 2) tmp.append("\n\n");
                else if (nl > 0) tmp.append(nl, '\n');
                nl = 0; tmp += c;
            }
        }
        if (nl > 2) tmp.append("\n\n");
        else if (nl > 0) tmp.append(nl, '\n');
        clean = std::move(tmp);
    }
    // 压缩行内空白
    {
        std::string tmp; tmp.reserve(clean.size());
        int sp = 0;
        for (char c : clean) {
            if (c == ' ' || c == '\t') { sp++; }
            else { if (sp > 0) { tmp += ' '; sp = 0; } tmp += c; }
        }
        clean = std::move(tmp);
    }
    // Trim
    clean.erase(0, clean.find_first_not_of(" \t\r\n"));
    clean.erase(clean.find_last_not_of(" \t\r\n") + 1);
    // UTF-8 清洗
    clean = sanitize_utf8(clean);

    const size_t max_return = 500 * 1024;
    std::string body = clean;
    if (clean.size() > max_return) {
        body = clean.substr(0, max_return);
        body += "\n\n[截断：" + std::to_string(clean.size() / 1024) + " KB，仅返回前 "
             + std::to_string(max_return / 1024) + " KB]";
    }
    return {{"url", orig_url}, {"size", clean.size()}, {"text", body}};
}

// ============================================================
// libcurl 回调
// ============================================================
static size_t curl_write_cb(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

// ============================================================
// HTTP 抓取纯函数
// ============================================================

// ============================================================
// URL 百分号编码（非 ASCII → %XX）
// ============================================================
static std::string url_encode(const std::string& url) {
    // 快速检测：如果没有非 ASCII 字符，直接返回
    bool needs_encode = false;
    for (unsigned char ch : url) {
        if (ch > 127) { needs_encode = true; break; }
    }
    if (!needs_encode) return url;

    std::string encoded;
    encoded.reserve(url.size() * 3);
    for (unsigned char ch : url) {
        if (ch > 127 || ch == ' ') {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", ch);
            encoded += buf;
        } else {
            encoded += ch;
        }
    }
    return encoded;
}

static json do_fetch(const std::string& url, int timeout_s, size_t max_size_bytes) {
    CURL* c = curl_easy_init();
    if (!c) return {{"error", "curl init failed"}};

    std::string html;
    html.reserve(1024 * 1024);
    curl_easy_setopt(c, CURLOPT_URL, url_encode(url).c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &html);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36");
    struct curl_slist* headers_fetch = nullptr;
    headers_fetch = curl_slist_append(headers_fetch, "Accept-Language: zh-CN,zh;q=0.9,en;q=0.5");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers_fetch);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(max_size_bytes));

    CURLcode rc = curl_easy_perform(c);
    long http_code = 0;
    char* effective_url = nullptr;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective_url);
    std::string final_url = effective_url ? effective_url : url;
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        std::string err = "curl: " + std::string(curl_easy_strerror(rc));
        if (rc == CURLE_FILESIZE_EXCEEDED) err += " (max_size exceeded)";
        return {{"error", err}};
    }
    if (http_code != 200)
        return {{"error", "HTTP " + std::to_string(http_code)}};

    return extract_text(html, final_url, url);
}

// ============================================================
// WebFetchNode
// ============================================================
class WebFetchNode : public rclcpp::Node {
public:
    explicit WebFetchNode(const std::string& agent_name)
        : Node("web_fetch_node", agent_name), agent_name_(agent_name)
    {
        declare_parameter("agent_name", agent_name);
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", 30.0);
        declare_parameter("max_size_mb", 5);

        info_rate_       = get_parameter("info_rate").as_double();
        default_timeout_ = get_parameter("default_timeout").as_double();
        max_size_bytes_  = get_parameter("max_size_mb").as_int() * 1024 * 1024;

        curl_global_init(CURL_GLOBAL_DEFAULT);

        std::string topic = "/" + agent_name_ + "/output/web_fetch/info";
        rclcpp::QoS qos(1);
        qos.transient_local();
        qos.reliable();
        info_pub_ = create_publisher<std_msgs::msg::String>(topic, qos);
        info_timer_ = create_wall_timer(
            std::chrono::milliseconds(static_cast<int>(1000.0 / info_rate_)),
            [this]() {
                auto msg = std_msgs::msg::String();
                msg.data = INFO_JSON;
                info_pub_->publish(msg);
            });

        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this,
            "/" + agent_name_ + "/output/web_fetch",
            // handle_goal
            [](const rclcpp_action::GoalUUID&,
               std::shared_ptr<const ExecuteTool::Goal>) {
                return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
            },
            // handle_cancel
            [this](auto goal_handle) { return handle_cancel(goal_handle); },
            // handle_accepted
            [this](auto goal_handle) { handle_accepted(goal_handle); });

        RCLCPP_INFO(get_logger(), "WebFetchNode ready. agent=%s max_size=%dMB",
                    agent_name_.c_str(), static_cast<int>(max_size_bytes_ / (1024*1024)));
    }

    ~WebFetchNode() override { curl_global_cleanup(); }

private:
    static constexpr const char* INFO_JSON = R"json({
  "type": "function",
  "function": {
    "name": "web_fetch",
    "description": "抓取指定 URL 的网页全文，返回提取后的纯文本内容（去除 HTML 标签和脚本）。\n\n参数:\n  - url: 要抓取的网页 URL（必填）\n  - timeout_sec: 超时秒数（可选，默认 30）",
    "parameters": {
      "type": "object",
      "required": ["url"],
      "properties": {
        "url": {"type": "string", "description": "要抓取的网页 URL"},
        "timeout_sec": {"type": "number", "description": "超时秒数（默认 30）"}
      }
    }
  }
})json";

    std::string agent_name_;
    double info_rate_, default_timeout_;
    size_t max_size_bytes_;
    std::atomic<bool> canceled_{false};
    std::thread work_thread_;

    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr info_timer_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;

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
    // handle_accepted — 验证输入后在线程中执行抓取
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
            result->output_json = "{\"error\":\"invalid input JSON\"}";
            result->exit_code = 1;
            goal_handle->abort(result);
            return;
        }

        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing arguments");
            result->output_json = "{\"error\":\"invalid input: missing arguments\"}";
            result->exit_code = 1;
            goal_handle->abort(result);
            return;
        }

        auto& args = input["arguments"];
        if (!args.contains("url") || !args["url"].is_string() ||
            args["url"].get<std::string>().empty()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing or empty url");
            result->output_json = "{\"error\":\"invalid input: url is required\"}";
            result->exit_code = 1;
            goal_handle->abort(result);
            return;
        }

        std::string url = args["url"];
        int timeout_s = static_cast<int>(default_timeout_);
        if (args.contains("timeout_sec") && args["timeout_sec"].is_number())
            timeout_s = args["timeout_sec"].get<int>();

        RCLCPP_INFO(get_logger(), "Fetching: %s (timeout=%ds)", url.substr(0, 80).c_str(), timeout_s);

        canceled_.store(false);
        size_t max_size = max_size_bytes_;
        work_thread_ = std::thread([goal_handle, url, timeout_s, max_size, this]() {
            auto t0 = std::chrono::steady_clock::now();
            json res = do_fetch(url, timeout_s, max_size);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

            if (canceled_.load()) {
                RCLCPP_INFO(get_logger(), "execute: Canceled before result");
                auto aborted = std::make_shared<ExecuteTool::Result>();
                goal_handle->abort(aborted);
                return;
            }

            auto result = std::make_shared<ExecuteTool::Result>();
            result->output_json = res.dump();
            result->exit_code = res.contains("error") ? 1 : 0;

            RCLCPP_INFO(get_logger(), "done in %ldms size=%d", ms,
                        res.value("size", 0));

            goal_handle->succeed(result);
        });
        work_thread_.detach();
    }
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
    auto node = std::make_shared<WebFetchNode>(agent_name);
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
