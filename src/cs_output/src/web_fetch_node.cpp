// Copyright (c) leaf
// SPDX-License-Identifier: MIT
//
// web_fetch_node — 抓取网页全文 (libcurl + HTML 转纯文本)

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <regex>
#include <sstream>

using json = nlohmann::json;
using ExecuteTool = cs_interfaces::action::ExecuteTool;

static size_t write_cb(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}


// UTF-8 清理：替换无效字节（nlohmann::json::dump 要求严格 UTF-8）
static std::string sanitize_utf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        unsigned char c = in[i];
        if (c < 0x80) { out += c; i++; }                    // ASCII
        else if (c < 0xC0) { out += '?'; i++; }             // 非法续字节
        else if (c < 0xE0) {                                 // 2字节
            if (i+1 < in.size() && (in[i+1] & 0xC0) == 0x80) { out += c; out += in[i+1]; }
            else out += '?';
            i += 2;
        } else if (c < 0xF0) {                               // 3字节
            if (i+2 < in.size() && (in[i+1] & 0xC0) == 0x80 && (in[i+2] & 0xC0) == 0x80) {
                out += c; out += in[i+1]; out += in[i+2];
            } else out += '?';
            i += 3;
        } else if (c < 0xF8) {                               // 4字节
            if (i+3 < in.size() && (in[i+1] & 0xC0) == 0x80 && (in[i+2] & 0xC0) == 0x80 && (in[i+3] & 0xC0) == 0x80) {
                out += c; out += in[i+1]; out += in[i+2]; out += in[i+3];
            } else out += '?';
            i += 4;
        } else { out += '?'; i++; }
    }
    return out;
}

// HTML 实体解码（与 web_search 共用逻辑）
static std::string decode_html_entities(const std::string& in) {
    std::string s = in;
    static const std::vector<std::pair<std::string, std::string>> ents = {
        {"&ensp;", " "}, {"&emsp;", " "}, {"&nbsp;", " "},
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"}, {"&middot;", "·"},
        {"&bull;", "·"}, {"&ndash;", "–"}, {"&mdash;", "—"},
    };
    for (const auto& [entity, ch] : ents) {
        size_t pos = 0;
        while ((pos = s.find(entity, pos)) != std::string::npos) {
            s.replace(pos, entity.size(), ch);
            pos += ch.size();
        }
    }
    std::regex num_ent(R"(&#(\d+);)");
    std::smatch m;
    while (std::regex_search(s, m, num_ent)) {
        int code = std::stoi(m[1].str());
        std::string ch;
        if (code < 128) ch = (char)code;
        else if (code < 0x800) {
            ch = (char)(0xC0 | (code >> 6));
            ch += (char)(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            ch = (char)(0xE0 | (code >> 12));
            ch += (char)(0x80 | ((code >> 6) & 0x3F));
            ch += (char)(0x80 | (code & 0x3F));
        }
        s.replace(m.position(), m.length(), ch);
    }
    return s;
}

// 去除 HTML 标签
static std::string strip_tags(const std::string& in) {
    return std::regex_replace(in, std::regex("<[^>]+>"), "");
}

// 去除脚本、样式块和 HTML 注释 — 手动状态机，避免 std::regex 栈溢出
static std::string strip_blocks(const std::string& in) {
    // 大小写不敏感的字符串搜索辅助
    auto icase_find = [&](size_t start, const std::string& needle) -> size_t {
        for (size_t i = start; i + needle.size() <= in.size(); i++) {
            bool ok = true;
            for (size_t j = 0; j < needle.size(); j++) {
                char c = in[i + j];
                char n = needle[j];
                if (c != n && c != (n ^ 0x20)) { ok = false; break; }
            }
            if (ok) return i;
        }
        return std::string::npos;
    };

    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '<') { out += in[i]; i++; continue; }

        // 尝试匹配 <script ...> ... </script>
        if (i + 7 < in.size()) {
            const char* p = in.c_str() + i + 1;
            if ((p[0]=='s'||p[0]=='S') && (p[1]=='c'||p[1]=='C') && (p[2]=='r'||p[2]=='R') &&
                (p[3]=='i'||p[3]=='I') && (p[4]=='p'||p[4]=='P') && (p[5]=='t'||p[5]=='T')) {
                size_t j = i + 7;
                while (j < in.size() && in[j] != '>') j++;
                if (j < in.size()) {
                    j++; // 跳过 >
                    size_t close = icase_find(j, "</script>");
                    if (close != std::string::npos) { i = close + 9; continue; }
                }
            }
        }

        // 尝试匹配 <style ...> ... </style>
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

        // 尝试匹配 HTML 注释 <!-- ... -->
        if (i + 3 < in.size() && in[i+1]=='!' && in[i+2]=='-' && in[i+3]=='-') {
            size_t close = in.find("-->", i + 4);
            if (close != std::string::npos) { i = close + 3; continue; }
        }

        out += in[i];
        i++;
    }
    return out;
}

class WebFetchNode : public rclcpp::Node {
public:
    WebFetchNode(const rclcpp::NodeOptions& opts = rclcpp::NodeOptions())
        : Node("web_fetch_node", opts) {

        declare_parameter("agent_name", "");
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", 30.0);
        declare_parameter("max_size_mb", 5);

        agent_name_     = get_parameter("agent_name").as_string();
        info_rate_      = get_parameter("info_rate").as_double();
        default_timeout_ = get_parameter("default_timeout").as_double();
        max_size_bytes_ = get_parameter("max_size_mb").as_int() * 1024 * 1024;

        if (agent_name_.empty()) {
            RCLCPP_FATAL(get_logger(), "agent_name 不能为空");
            rclcpp::shutdown();
        }

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
            this, "/" + agent_name_ + "/output/web_fetch",
            [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
            [](auto...) { return rclcpp_action::CancelResponse::ACCEPT; },
            [this](auto h) { handle_accepted(h); });

        RCLCPP_INFO(get_logger(), "web_fetch 启动: agent=%s, max_size=%dMB",
            agent_name_.c_str(), (int)(max_size_bytes_ / (1024*1024)));
    }

    ~WebFetchNode() { curl_global_cleanup(); }

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

    void handle_accepted(std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> gh) {
        auto goal = gh->get_goal();
        json input;
        try { input = json::parse(goal->input_json); } catch (...) {
            fail(gh, R"({"error":"invalid JSON"})"); return;
        }
        if (!input.contains("name") || !input["name"].is_string() || input["name"] != "web_fetch") {
            fail(gh, R"({"error":"invalid tool name"})"); return;
        }
        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            fail(gh, R"({"error":"missing arguments"})"); return;
        }
        if (!input["arguments"].contains("url") || !input["arguments"]["url"].is_string()) {
            fail(gh, R"({"error":"url is required"})"); return;
        }

        std::string url = input["arguments"]["url"];
        double timeout = default_timeout_;
        if (input["arguments"].contains("timeout_sec") && input["arguments"]["timeout_sec"].is_number()) {
            double t = input["arguments"]["timeout_sec"].get<double>();
            if (t > 0) timeout = t;
        }

        RCLCPP_INFO(get_logger(), "Fetching: %s", url.substr(0, 80).c_str());
        json resp = do_fetch(url, timeout);

        auto r = std::make_shared<ExecuteTool::Result>();
        r->output_json = resp.dump();
        r->exit_code = resp.contains("error") ? -1 : 0;
        gh->succeed(r);
    }

    void fail(std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> gh, const std::string& msg) {
        auto r = std::make_shared<ExecuteTool::Result>();
        r->output_json = msg; r->exit_code = -1;
        gh->abort(r);
    }

    json do_fetch(const std::string& url, double timeout_sec) {
        CURL* curl = curl_easy_init();
        if (!curl) return {{"error", "curl init failed"}};

        std::string html;
        html.reserve(1024 * 1024); // 预分配 1MB

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)max_size_bytes_);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        char* final_url = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::string err = "curl: " + std::string(curl_easy_strerror(res));
            if (res == CURLE_FILESIZE_EXCEEDED)
                err += " (max_size exceeded)";
            return {{"error", err}};
        }

        if (http_code != 200) {
            return {{"error", "HTTP " + std::to_string(http_code)}};
        }

        return extract_text(html, final_url ? final_url : url, url);
    }

    json extract_text(const std::string& html, const std::string& final_url, const std::string& orig_url) {
        std::string clean = strip_blocks(html);
        clean = strip_tags(clean);
        clean = decode_html_entities(clean);

        // 压缩空白行：多个连续换行 → 最多两个
        clean = std::regex_replace(clean, std::regex("\n{3,}"), "\n\n");
        // 压缩行内空白
        clean = std::regex_replace(clean, std::regex("[ \\t]{2,}"), " ");
        // trim 首尾
        clean.erase(0, clean.find_first_not_of(" \t\r\n"));
        clean.erase(clean.find_last_not_of(" \t\r\n") + 1);

        // UTF-8 清洗（防 nlohmann::json::dump 崩溃）
        clean = sanitize_utf8(clean);

        // 限制返回大小（最多 500KB，保护 LLM 上下文）
        const size_t max_return = 500 * 1024;
        std::string body = clean;
        if (clean.size() > max_return) {
            body = clean.substr(0, max_return);
            body += "\n\n[截断：" + std::to_string(clean.size() / 1024) + " KB，仅返回前 "
                 + std::to_string(max_return / 1024) + " KB]";
        }

        return {
            {"url", orig_url},
            {"size", clean.size()},
            {"text", body}
        };
    }

    std::string agent_name_;
    double info_rate_, default_timeout_;
    size_t max_size_bytes_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr info_timer_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<WebFetchNode>());
    rclcpp::shutdown();
    return 0;
}
