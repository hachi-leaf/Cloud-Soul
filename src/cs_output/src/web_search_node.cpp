// Copyright (c) leaf
// SPDX-License-Identifier: MIT
//
// web_search_node — 纯 C++ Bing 网页搜索 (libcurl + regex)

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
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

static size_t write_cb(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// ─── HTML 实体解码 ──────────────────────────────────────────
static std::string decode_html_entities(const std::string& in) {
    std::string s = in;
    // 常见 HTML 实体
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
    // 数字实体 &#0183; &#1234;
    std::regex num_ent(R"(&#(\d+);)");
    std::smatch m;
    while (std::regex_search(s, m, num_ent)) {
        int code = std::stoi(m[1].str());
        std::string ch;
        if (code < 128) ch = (char)code;
        else {
            // UTF-8 encode
            if (code < 0x800) {
                ch = (char)(0xC0 | (code >> 6));
                ch += (char)(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                ch = (char)(0xE0 | (code >> 12));
                ch += (char)(0x80 | ((code >> 6) & 0x3F));
                ch += (char)(0x80 | (code & 0x3F));
            }
        }
        s.replace(m.position(), m.length(), ch);
    }
    return s;
}

// ─── 去除 HTML 标签 ─────────────────────────────────────────
static std::string strip_tags(const std::string& in) {
    return std::regex_replace(in, std::regex("<[^>]+>"), "");
}

// ─── 清理文本 ───────────────────────────────────────────────
static std::string clean_text(const std::string& in) {
    std::string s = strip_tags(in);
    s = decode_html_entities(s);
    // 压缩空白
    s = std::regex_replace(s, std::regex("\\s+"), " ");
    // trim
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

class WebSearchNode : public rclcpp::Node {
public:
    WebSearchNode(const rclcpp::NodeOptions& opts = rclcpp::NodeOptions())
        : Node("web_search_node", opts) {

        declare_parameter("agent_name", "");
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", 30.0);
        declare_parameter("max_results", 10);

        agent_name_     = get_parameter("agent_name").as_string();
        info_rate_      = get_parameter("info_rate").as_double();
        default_timeout_ = get_parameter("default_timeout").as_double();
        max_results_    = get_parameter("max_results").as_int();

        if (agent_name_.empty()) {
            RCLCPP_FATAL(get_logger(), "agent_name 不能为空");
            rclcpp::shutdown();
        }

        curl_global_init(CURL_GLOBAL_DEFAULT);

        std::string topic = "/" + agent_name_ + "/output/web_search/info";
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
            this, "/" + agent_name_ + "/output/web_search",
            [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
            [](auto...) { return rclcpp_action::CancelResponse::ACCEPT; },
            [this](auto h) { handle_accepted(h); });

        RCLCPP_INFO(get_logger(), "web_search 启动: agent=%s, max=%d",
            agent_name_.c_str(), max_results_);
    }

    ~WebSearchNode() { curl_global_cleanup(); }

private:
    static constexpr const char* INFO_JSON = R"json({
  "type": "function",
  "function": {
    "name": "web_search",
    "description": "网页搜索，返回标题、URL 和摘要。\n\n参数:\n  - query: 搜索查询字符串（必填）\n  - max_results: 最大结果数（可选，默认10，最大20）\n  - source: 搜索引擎（可选，默认 bing）。可选值: bing, auto",
    "parameters": {
      "type": "object",
      "required": ["query"],
      "properties": {
        "query": {"type": "string", "description": "搜索查询字符串"},
        "max_results": {"type": "integer", "description": "最大结果数（默认10，最大20）"},
        "source": {"type": "string", "description": "搜索引擎: bing（默认）, auto（自动选最快）"}
      }
    }
  }
})json";

    void handle_accepted(std::shared_ptr<GoalHandleExecute> gh) {
        auto goal = gh->get_goal();
        json input;
        try { input = json::parse(goal->input_json); } catch (...) {
            fail(gh, R"({"error":"invalid JSON"})"); return;
        }
        if (!input.contains("name") || !input["name"].is_string() || input["name"] != "web_search") {
            fail(gh, R"({"error":"invalid tool name"})"); return;
        }
        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            fail(gh, R"({"error":"missing arguments"})"); return;
        }
        if (!input["arguments"].contains("query") || !input["arguments"]["query"].is_string()) {
            fail(gh, R"({"error":"query is required"})"); return;
        }

        std::string query = input["arguments"]["query"];
        int n = max_results_;
        if (input["arguments"].contains("max_results") && input["arguments"]["max_results"].is_number())
            n = std::min(input["arguments"]["max_results"].get<int>(), 20);

        std::string source = "bing";
        if (input["arguments"].contains("source") && input["arguments"]["source"].is_string())
            source = input["arguments"]["source"];

        double timeout = default_timeout_;
        if (input["arguments"].contains("timeout_sec") && input["arguments"]["timeout_sec"].is_number()) {
            double t = input["arguments"]["timeout_sec"].get<double>();
            if (t > 0) timeout = t;
        }

        json resp = do_search(query, n, source, timeout);

        auto r = std::make_shared<ExecuteTool::Result>();
        r->output_json = resp.dump();
        r->exit_code = resp.contains("error") ? -1 : 0;
        gh->succeed(r);
    }

    void fail(std::shared_ptr<GoalHandleExecute> gh, const std::string& msg) {
        auto r = std::make_shared<ExecuteTool::Result>();
        r->output_json = msg; r->exit_code = -1;
        gh->abort(r);
    }

    // ─── 执行搜索 ───────────────────────────────────────────
    json do_search(const std::string& query, int n, const std::string& source, double timeout_sec) {
        // source: "bing" or "auto" — currently only Bing is available
        CURL* curl = curl_easy_init();
        if (!curl) return {{"error", "curl init failed"}};

        char* enc = curl_easy_escape(curl, query.c_str(), query.size());
        std::string url = "https://www.bing.com/search?q=" + std::string(enc) + "&count=" + std::to_string(n);
        curl_free(enc);

        std::string html;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
            return {{"error", std::string("curl: ") + curl_easy_strerror(res)}};

        return parse_results(html, n);
    }

    // ─── 解析 Bing HTML ────────────────────────────────────
    json parse_results(const std::string& html, int n) {
        json results = json::array();
        try {
            std::regex block_re(R"(<li class=\"b_algo\"[^>]*>(.*?)</li>)",
                std::regex::icase | std::regex::optimize);
            std::regex h2_re(R"(<h2[^>]*>(.*?)</h2>)", std::regex::icase);
            std::regex link_re(R"(<a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>)", std::regex::icase);
            std::regex snip_re(R"(<p[^>]*>(.*?)</p>)", std::regex::icase);

            auto it = std::sregex_iterator(html.begin(), html.end(), block_re);
            auto end = std::sregex_iterator();

            for (; it != end && (int)results.size() < n; ++it) {
                std::string block = (*it)[1].str();

                // 从 h2 提取标题 (不是 favicon 的 a 标签)
                std::smatch h2m;
                if (!std::regex_search(block, h2m, h2_re)) continue;
                std::string h2_content = h2m[1].str();

                std::smatch lm;
                if (!std::regex_search(h2_content, lm, link_re)) continue;
                std::string url = lm[1].str();
                std::string title = clean_text(lm[2].str());

                // 摘要
                std::smatch sm;
                std::string snippet;
                if (std::regex_search(block, sm, snip_re))
                    snippet = clean_text(sm[1].str());

                if (!url.empty() && !title.empty()) {
                    results.push_back({
                        {"title", title},
                        {"url", url},
                        {"snippet", snippet}
                    });
                }
            }
        } catch (const std::exception& e) {
            RCLCPP_WARN(get_logger(), "parse error: %s", e.what());
        }

        if (results.empty())
            return {{"error", "no results"}, {"results", json::array()}};
        return {{"results", results}, {"count", results.size()}};
    }

    std::string agent_name_;
    double info_rate_, default_timeout_;
    int max_results_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr info_timer_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<WebSearchNode>());
    rclcpp::shutdown();
    return 0;
}
