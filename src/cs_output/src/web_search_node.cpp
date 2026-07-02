// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// ================================================================
// Cloud-Soul Web 搜索工具节点
// ================================================================
//
// 作用:
//   Bing 网页搜索工具，通过 libcurl 抓取搜索页面 HTML，用正则提取标题、
//   URL 和摘要。线程化执行，支持取消和超时。
//
// 节点名: /<agent_name>/web_search_node
//
// 参数:
//   agent_name       (string, 必填)  Agent 命名空间
//   info_rate         (double, 1.0)  发布 Tools Info 的频率（Hz）
//   default_timeout   (double, 30.0)  搜索默认超时秒数
//   cancel_timeout    (double, 5.0)   取消等待秒数
//   max_results       (int, 10)      最大结果数
//
// Action:
//   /<agent_name>/output/web_search  (ExecuteTool)
//     Goal: 接收 {"name":"web_search","arguments":{"query":"...","max_results":10,"timeout_sec":30}}
//     Result: output_json 为 {"results":[{url,title,snippet},...],"count":N}
//     Cancel: 终止正在进行的 HTTP 请求
//
// 上层传入 JSON 规范 (来自 output_mgmt):
//   output_mgmt 透传以下 JSON 给本节点:
//   {
//     "name": "web_search",
//     "arguments": {
//       "query": "搜索关键词",               // 必填，搜索查询
//       "max_results": 10,                   // 可选，最大结果数
//       "timeout_sec": 30                    // 可选，超时秒数
//     }
//   }
//
// 关键设计:
//   - 从 arguments 子对象提取参数（与 output_mgmt 接收格式一致）
//   - 线程化执行，支持取消
//   - 无需并行保护（output_mgmt 通过 busy 标志保证同一时间只有一个 Goal）
//   - HTML 实体解码（&amp; &lt; &gt; &quot; &#39; &nbsp; &ensp;）
//   - 拆分纯函数 do_search，便于测试
//

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
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

using json = nlohmann::json;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using namespace std::chrono_literals;

// ============================================================
// HTML 标签清理 & 实体解码
// ============================================================
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
    replace("&ensp;", " ");
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

// Bing 搜索结果正则
static const char* BING_PATTERN =
    "<li class=\"b_algo\"[^>]*>.*?<h2[^>]*>.*?<a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>.*?<p[^>]*>(.*?)</p>";

// ============================================================
// libcurl 回调
// ============================================================
static size_t curl_write_cb(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

// ============================================================
// Bing 搜索纯函数（在线程中调用）
// ============================================================
static json do_search(const std::string& query, int max_r, int timeout_s,
                      const std::string& proxy) {
    json result;
    result["results"] = json::array();
    result["count"] = 0;

    CURL* c = curl_easy_init();
    if (!c) { result["error"] = "curl init failed"; return result; }

    std::string url = "https://cn.bing.com/search?q=";
    char* esc = curl_easy_escape(c, query.c_str(), query.size());
    url += esc;
    curl_free(esc);

    std::string body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    struct curl_slist* headers_srch = nullptr;
    headers_srch = curl_slist_append(headers_srch, "Accept-Language: zh-CN,zh;q=0.9,en;q=0.5");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers_srch);
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

// ============================================================
// WebSearchNode
// ============================================================
class WebSearchNode : public rclcpp::Node {
public:
    
    static constexpr const char* INFO_JSON = R"json({
  "type": "function",
  "function": {
    "name": "web_search",
    "description": "使用 Bing 搜索引擎搜索网页，返回标题、URL 和摘要。",
    "parameters": {
      "type": "object",
      "required": ["query"],
      "properties": {
        "query": {
          "type": "string",
          "description": "搜索关键词"
        },
        "max_results": {
          "type": "integer",
          "description": "最大结果数，默认 10"
        },
        "timeout_sec": {
          "type": "integer",
          "description": "超时秒数，默认 30"
        }
      }
    }
  }
})json";

    explicit WebSearchNode(const std::string& agent_name)
        : Node("web_search_node", agent_name), agent_name_(agent_name)
    {
        declare_parameter("agent_name", agent_name);
        declare_parameter("info_rate", 1.0);
        declare_parameter("default_timeout", 30.0);
        declare_parameter("cancel_timeout", 5.0);
        declare_parameter("max_results", 10);

        info_rate_ = get_parameter("info_rate").as_double();
        default_timeout_ = get_parameter("default_timeout").as_double();
        cancel_timeout_ = get_parameter("cancel_timeout").as_double();
        max_results_ = get_parameter("max_results").as_int();

        const char* proxy = std::getenv("HTTPS_PROXY");
        if (!proxy) proxy = std::getenv("https_proxy");
        proxy_ = proxy ? std::string(proxy) : "";

        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this,
            "/" + agent_name_ + "/output/web_search",
            // handle_goal
            [this](const rclcpp_action::GoalUUID&,
                   std::shared_ptr<const ExecuteTool::Goal>) {
                return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
            },
            // handle_cancel
            [this](auto goal_handle) { return handle_cancel(goal_handle); },
            // handle_accepted
            [this](auto goal_handle) { handle_accepted(goal_handle); });

        // Info 发布
        std::string topic = "/" + agent_name_ + "/output/web_search/info";
        rclcpp::QoS qos(1);
        qos.transient_local();
        qos.reliable();
        info_pub_ = create_publisher<std_msgs::msg::String>(topic, qos);

        publish_timer_ = create_wall_timer(
            std::chrono::duration<double>(1.0 / info_rate_),
            [this]() {
                std_msgs::msg::String msg;
                msg.data = INFO_JSON;
                info_pub_->publish(msg);
            });

        RCLCPP_INFO(get_logger(), "WebSearchNode ready. agent=%s proxy=%s",
                    agent_name_.c_str(), proxy_.empty() ? "none" : proxy_.c_str());
    }

private:
    std::string agent_name_;
    double info_rate_;
    double default_timeout_;
    double cancel_timeout_;
    int max_results_;
    std::string proxy_;
    std::atomic<bool> canceled_{false};
    std::thread work_thread_;

    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr publish_timer_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;

    // ---------------------------------------------------------
    // handle_cancel — 设置 canceled_ 标志
    // ---------------------------------------------------------
    rclcpp_action::CancelResponse handle_cancel(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        RCLCPP_INFO(get_logger(), "handle_cancel: Cancel requested");
        canceled_.store(true);
        return rclcpp_action::CancelResponse::ACCEPT;
    }

    // ---------------------------------------------------------
    // handle_accepted — 验证输入后在线程中执行搜索
    // ---------------------------------------------------------
    void handle_accepted(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle)
    {
        const auto goal = goal_handle->get_goal();
        auto result = std::make_shared<ExecuteTool::Result>();

        // 解析 input_json
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

        // 校验 arguments 存在且为对象
        if (!input.contains("arguments") || !input["arguments"].is_object()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing arguments");
            result->output_json = "{\"error\":\"invalid input: missing arguments\"}";
            result->exit_code = 1;
            goal_handle->abort(result);
            return;
        }

        auto& args = input["arguments"];

        // 校验 query 必填
        if (!args.contains("query") || !args["query"].is_string() ||
            args["query"].get<std::string>().empty()) {
            RCLCPP_ERROR(get_logger(), "handle_accepted: Missing or empty query");
            result->output_json = "{\"error\":\"invalid input: query is required\"}";
            result->exit_code = 1;
            goal_handle->abort(result);
            return;
        }

        // 提取参数
        std::string query = args["query"];
        int max_r = max_results_;
        int timeout_s = static_cast<int>(default_timeout_);

        if (args.contains("max_results") && args["max_results"].is_number()) {
            max_r = args["max_results"].get<int>();
        }
        if (args.contains("timeout_sec") && args["timeout_sec"].is_number()) {
            timeout_s = args["timeout_sec"].get<int>();
        }

        RCLCPP_INFO(get_logger(), "search query='%s' max=%d timeout=%d",
                    query.c_str(), max_r, timeout_s);

        // 在独立线程中执行搜索
        canceled_.store(false);
        execute(goal_handle, query, max_r, timeout_s);
    }

    // ---------------------------------------------------------
    // execute — 在新线程中执行搜索（不阻塞 ROS2 executor）
    // ---------------------------------------------------------
    void execute(
        const std::shared_ptr<rclcpp_action::ServerGoalHandle<ExecuteTool>> goal_handle,
        const std::string& query, int max_r, int timeout_s)
    {
        std::string proxy = proxy_;
        work_thread_ = std::thread([goal_handle, query, max_r, timeout_s, proxy, this]() {
            auto t0 = std::chrono::steady_clock::now();
            json res = do_search(query, max_r, timeout_s, proxy);
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

            RCLCPP_INFO(get_logger(), "done in %ldms results=%d",
                        ms, res.value("count", 0));

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
    auto node = std::make_shared<WebSearchNode>(agent_name);
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
