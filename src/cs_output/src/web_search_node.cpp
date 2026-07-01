// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// ================================================================
// Cloud-Soul Web 搜索工具节点
// Bing 搜索引擎，线程化执行，支持取消
// ================================================================

#include <curl/curl.h>
#include <regex>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using json = nlohmann::json;
using ExecuteTool = cs_interfaces::action::ExecuteTool;

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
    replace("&ensp;", " ");  // Bing 用的半角空格实体
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

// Bing 搜索页面的正则模式
static const char* BING_PATTERN =
    "<li class=\"b_algo\"[^>]*>.*?<h2[^>]*>.*?<a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>.*?<p[^>]*>(.*?)</p>";

static size_t curl_write_cb(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

static std::string err_json(const std::string& msg) {
    return "{\"error\":\"" + msg + "\"}";
}

static json do_search(const std::string& query, int max_r, int timeout_s,
                      const std::string& proxy) {
    json result;
    result["results"] = json::array();
    result["count"] = 0;

    CURL* c = curl_easy_init();
    if (!c) { result["error"] = "curl init failed"; return result; }

    std::string url = "https://www.bing.com/search?q=";
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
    if (!proxy.empty()) curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());

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

    std::regex re(BING_PATTERN, std::regex::icase);
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

    void execute(std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> handle) {
        cancelled_ = false;
        auto goal = handle->get_goal();

        json input;
        try { input = json::parse(goal->input_json); } catch (...) {
            auto r = std::make_shared<ExecuteTool::Result>();
            r->output_json = err_json("invalid input JSON");
            handle->succeed(r);
            return;
        }

        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            auto r = std::make_shared<ExecuteTool::Result>();
            r->output_json = err_json("missing arguments");
            handle->succeed(r);
            return;
        }

        auto& args = input["arguments"];
        if (!args.contains("query") || !args["query"].is_string()) {
            auto r = std::make_shared<ExecuteTool::Result>();
            r->output_json = err_json("missing query");
            handle->succeed(r);
            return;
        }

        std::string query = args["query"];
        int max_r     = args.value("max_results", max_results_);
        int timeout_s = args.value("timeout_sec", static_cast<int>(default_timeout_));

        RCLCPP_INFO(this->get_logger(), "search query='%s' max=%d timeout=%d",
                    query.c_str(), max_r, timeout_s);

        auto safe_handle = handle;
        std::string proxy = proxy_;
        std::thread([=, this]() {
            auto t0 = std::chrono::steady_clock::now();
            json res = do_search(query, max_r, timeout_s, proxy);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

            if (cancelled_) {
                auto ar = std::make_shared<ExecuteTool::Result>();
                ar->output_json = err_json("cancelled");
                safe_handle->abort(ar);
                return;
            }

            auto sr = std::make_shared<ExecuteTool::Result>();
            sr->output_json = res.dump();
            sr->exit_code = res.contains("error") ? 1 : 0;
            RCLCPP_INFO(this->get_logger(), "done in %ldms results=%d",
                        ms, res.value("count", 0));
            safe_handle->succeed(sr);
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
