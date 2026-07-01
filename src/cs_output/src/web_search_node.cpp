// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// ================================================================
// Cloud-Soul Web 搜索工具节点 (v2)
// 多引擎搜索 (bing/360/sogou/auto)，线程化执行，支持取消
// ================================================================

#include <curl/curl.h>
#include <regex>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using json = nlohmann::json;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using namespace std::chrono_literals;

static std::string strip_html(const std::string& in) {
    std::string out;
    bool in_tag = false;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '<') in_tag = true;
        else if (in[i] == '>') { in_tag = false; continue; }
        else if (!in_tag) out += in[i];
    }
    auto replace = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace("&amp;", "&");  replace("&lt;", "<");   replace("&gt;", ">");
    replace("&quot;", "\""); replace("&#39;", "'"); replace("&nbsp;", " ");
    std::string cleaned;
    bool prev_space = true;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!prev_space) cleaned += ' ';
            prev_space = true;
        } else { cleaned += c; prev_space = false; }
    }
    return cleaned;
}

struct EngineConfig {
    std::string name;
    std::string host;
    std::string url_prefix;
    std::string result_pattern;
};

static const std::vector<EngineConfig> ENGINES = {
    {"bing",  "www.bing.com",
     "https://www.bing.com/search?q=",
     "<li class=\"b_algo\"[^>]*>.*?<h2>.*?<a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>.*?<p[^>]*>(.*?)</p>"},
    {"so360", "www.so.com",
     "https://www.so.com/s?q=",
     "<li class=\"res-list\"[^>]*>.*?<h3[^>]*>.*?<a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>.*?<p class=\"res-desc\"[^>]*>(.*?)</p>"},
    {"sogou", "www.sogou.com",
     "https://www.sogou.com/web?query=",
     "<div class=\"vrwrap\"[^>]*>.*?<h3[^>]*>.*?<a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>.*?<p class=\"star-wiki\"[^>]*>(.*?)</p>"},
};

static size_t curl_write_cb(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

// 简单 JSON 错误响应构造（避免 raw string 的转义地狱）
static std::string err_json(const std::string& msg) {
    return "{\"error\":\"" + msg + "\"}";
}

class WebSearchNode : public rclcpp::Node {
public:
    WebSearchNode() : Node("web_search_node") {
        this->declare_parameter("agent_name", "");
        this->declare_parameter("info_rate", 1.0);
        this->declare_parameter("default_timeout", 30.0);
        this->declare_parameter("cancel_timeout", 5.0);
        this->declare_parameter("max_results", 10);

        agent_name_ = this->get_parameter("agent_name").as_string();
        default_timeout_ = this->get_parameter("default_timeout").as_double();
        max_results_ = this->get_parameter("max_results").as_int();

        if (agent_name_.empty())
            throw std::runtime_error("agent_name parameter is required");

        const char* proxy = std::getenv("HTTPS_PROXY");
        if (!proxy) proxy = std::getenv("https_proxy");
        proxy_ = proxy ? std::string(proxy) : "";

        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this, "/" + agent_name_ + "/output/web_search",
            [this](const rclcpp_action::GoalUUID&, std::shared_ptr<const ExecuteTool::Goal>) {
                return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
            },
            [this](std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>>) {
                RCLCPP_INFO(this->get_logger(), "cancel requested");
                cancelled_ = true;
                return rclcpp_action::CancelResponse::ACCEPT;
            },
            [this](std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> h) {
                execute(h);
            });

        RCLCPP_INFO(this->get_logger(), "WebSearchNode ready. agent=%s proxy=%s",
                    agent_name_.c_str(), proxy_.empty() ? "none" : proxy_.c_str());
    }

private:
    std::string agent_name_;
    double default_timeout_;
    int max_results_;
    std::string proxy_;
    std::atomic<bool> cancelled_{false};
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;

    static int check_latency_ms(const std::string& host, const std::string& proxy) {
        CURL* c = curl_easy_init();
        if (!c) return -1;
        std::string url = "https://" + host + "/";
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        if (!proxy.empty()) curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
        CURLcode res = curl_easy_perform(c);
        long http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        double total = 0;
        curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &total);
        curl_easy_cleanup(c);
        return (res == CURLE_OK && http_code > 0 && http_code < 500)
            ? static_cast<int>(total * 1000) : -1;
    }

    int select_engine(const std::string& source) {
        if (source == "auto") {
            int best_idx = 0, best_lat = 999999;
            for (size_t i = 0; i < ENGINES.size(); ++i) {
                int lat = check_latency_ms(ENGINES[i].host, proxy_);
                RCLCPP_INFO(this->get_logger(), "engine=%s latency=%dms",
                            ENGINES[i].name.c_str(), lat);
                if (lat >= 0 && lat < best_lat) { best_lat = lat; best_idx = i; }
            }
            RCLCPP_INFO(this->get_logger(), "auto selected engine=%s (%dms)",
                        ENGINES[best_idx].name.c_str(), best_lat);
            return best_idx;
        }
        for (size_t i = 0; i < ENGINES.size(); ++i)
            if (ENGINES[i].name == source) return i;
        return 0;
    }

    json do_search(const EngineConfig& eng, const std::string& query,
                   int max_r, int timeout_s) {
        json result;
        result["results"] = json::array();
        result["count"] = 0;

        CURL* c = curl_easy_init();
        if (!c) { result["error"] = "curl init failed"; return result; }

        std::string url = eng.url_prefix;
        char* esc = curl_easy_escape(c, query.c_str(), query.size());
        url += esc; curl_free(esc);

        std::string body;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT,
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        if (!proxy_.empty()) curl_easy_setopt(c, CURLOPT_PROXY, proxy_.c_str());

        CURLcode rc = curl_easy_perform(c);
        long http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(c);

        if (rc != CURLE_OK) {
            result["error"] = std::string("curl: ") + curl_easy_strerror(rc);
            return result;
        }
        if (http_code != 200) {
            result["error"] = "HTTP " + std::to_string(http_code);
            return result;
        }

        std::regex re(eng.result_pattern, std::regex::icase);
        auto it = std::sregex_iterator(body.begin(), body.end(), re);
        auto end = std::sregex_iterator();
        for (int k = 0; it != end && k < max_r; ++it, ++k) {
            auto& m = *it;
            if (m.size() < 4) continue;
            json item;
            item["url"]     = m[1].str();
            item["title"]   = strip_html(m[2].str());
            item["snippet"] = strip_html(m[3].str());
            result["results"].push_back(item);
        }
        result["count"] = result["results"].size();
        return result;
    }

    void execute(std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> handle) {
        cancelled_ = false;
        auto goal = handle->get_goal();
        auto result = std::make_shared<ExecuteTool::Result>();

        // 解析 input_json: {"name":"web_search","arguments":{...}}
        json input;
        try {
            input = json::parse(goal->input_json);
        } catch (...) {
            result->output_json = err_json("invalid input JSON");
            handle->succeed(result);
            return;
        }

        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            result->output_json = err_json("missing arguments");
            handle->succeed(result);
            return;
        }

        auto& args = input["arguments"];
        if (!args.contains("query") || !args["query"].is_string()) {
            result->output_json = err_json("missing query");
            handle->succeed(result);
            return;
        }

        std::string query   = args["query"];
        int    max_r    = args.value("max_results", max_results_);
        std::string source = args.value("source", "auto");
        int    timeout_s = args.value("timeout_sec", static_cast<int>(default_timeout_));

        RCLCPP_INFO(this->get_logger(), "search query='%s' engine=%s max=%d timeout=%d",
                    query.c_str(), source.c_str(), max_r, timeout_s);

        int ei = select_engine(source);
        const auto& eng = ENGINES[ei];

        auto safe_handle = handle;
        auto safe_result = result;
        std::thread([=, this]() {
            auto t0 = std::chrono::steady_clock::now();
            json res = do_search(eng, query, max_r, timeout_s);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

            if (cancelled_) {
                auto ar = std::make_shared<ExecuteTool::Result>();
                ar->output_json = err_json("cancelled");
                safe_handle->abort(ar);
                return;
            }

            safe_result->output_json = res.dump();
            safe_result->exit_code = res.contains("error") ? 1 : 0;
            RCLCPP_INFO(this->get_logger(), "done in %ldms results=%d engine=%s",
                        ms, res.value("count", 0), eng.name.c_str());
            safe_handle->succeed(safe_result);
        }).detach();
    }
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    try {
        rclcpp::spin(std::make_shared<WebSearchNode>());
    } catch (const std::exception& e) {
        RCLCPP_ERROR(rclcpp::get_logger("web_search_node"), "Fatal: %s", e.what());
        return 1;
    }
    rclcpp::shutdown();
    return 0;
}
