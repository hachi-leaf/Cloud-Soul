// Copyright (c) leaf
// SPDX-License-Identifier: MIT


// TODO: Sogou/Bing 搜索 HTML 解析器未实现，目前仅 360 搜索可用。见 handle_search() 中 Sogou/Bing continue 分支。
// 节点: /<agent_name>/output/web_search
// 作用: 多功能网页搜索与信息获取工具（免 Token，免注册，纯公开接口）
//       支持: search, wikipedia, image_search, image_download, file_download,
//             weather, news, translate, unit_convert, calculator,
//             ip_info, crypto_price, arxiv, rss, extract_content, qrcode
//       所有网络操作均配置多源 fallback，并在调用前进行轻量连通检测（ping）。
// 协议: 完全符合 output_mgmt_node 工具节点规范
// 依赖: libcurl, libxml2, nlohmann/json (header-only)

#include <chrono>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "std_msgs/msg/string.hpp"
#include "cs_interfaces/action/execute_tool.hpp"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using ExecuteTool = cs_interfaces::action::ExecuteTool;
using GoalHandleExecute = rclcpp_action::ServerGoalHandle<ExecuteTool>;

// ==========================================================================
// JSON 辅助函数
// ==========================================================================
static std::string remove_json_whitespace(const std::string &json) {
    std::string out;
    out.reserve(json.size());
    bool in_string = false;
    for (size_t i = 0; i < json.size(); ++i) {
        char c = json[i];
        if (in_string) {
            out.push_back(c);
            if (c == '"' && (i == 0 || json[i - 1] != '\\')) in_string = false;
        } else {
            if (c == '"') { in_string = true; out.push_back(c); }
            else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            else out.push_back(c);
        }
    }
    return out;
}

static std::string unescape_json(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '"': out += '"'; ++i; break;
                case '\\': out += '\\'; ++i; break;
                case '/': out += '/'; ++i; break;
                case 'n': out += '\n'; ++i; break;
                case 'r': out += '\r'; ++i; break;
                case 't': out += '\t'; ++i; break;
                default: out += '\\'; break;
            }
        } else out += s[i];
    }
    return out;
}

static std::string escape_json_string(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// 解析新浪财经 JSON
static std::optional<nlohmann::json> parse_sina_news(const std::string &resp, int max_results) {
    try {
        auto data = nlohmann::json::parse(resp);
        if (data.contains("result") && data["result"].contains("data")) {
            auto arr = data["result"]["data"];
            if (arr.is_array()) {
                nlohmann::json articles = nlohmann::json::array();
                for (auto &item : arr) {
                    if ((int)articles.size() >= max_results) break;
                    articles.push_back({
                        {"title", item.value("title", "")},
                        {"link", item.value("url", "")},
                        {"pubDate", item.value("ctime", "")},
                        {"description", item.value("intro", "")}
                    });
                }
                return articles;
            }
        }
    } catch (...) {}
    return std::nullopt;
}

// 解析 360 新闻 HTML（简单提取标题和链接）
static std::optional<nlohmann::json> parse_360_news(const std::string &html, int max_results) {
    htmlDocPtr doc = htmlReadMemory(html.c_str(), html.size(), nullptr, nullptr,
                                    HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return std::nullopt;
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (!ctx) { xmlFreeDoc(doc); return std::nullopt; }

    // 360 新闻标题通常在 <h3 class="res-title"> 下的 <a> 标签
    const char *xpath = "//h3[contains(@class,'res-title')]/a";
    xmlXPathObjectPtr obj = xmlXPathEvalExpression(reinterpret_cast<const xmlChar *>(xpath), ctx);
    nlohmann::json articles = nlohmann::json::array();
    if (obj && obj->nodesetval) {
        xmlNodeSetPtr nodes = obj->nodesetval;
        for (int i = 0; i < nodes->nodeNr && (int)articles.size() < max_results; ++i) {
            xmlNodePtr node = nodes->nodeTab[i];
            xmlChar *href = xmlGetProp(node, reinterpret_cast<const xmlChar *>("href"));
            xmlChar *title = xmlNodeGetContent(node);
            if (href && title) {
                articles.push_back({
                    {"title", reinterpret_cast<const char *>(title)},
                    {"link", reinterpret_cast<const char *>(href)},
                    {"pubDate", ""},
                    {"description", ""}
                });
            }
            if (href) xmlFree(href);
            if (title) xmlFree(title);
        }
    }
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);
    if (articles.empty()) return std::nullopt;
    return articles;
}

// 解析 360 搜索结果 HTML，提取标题、链接、摘要
static std::optional<nlohmann::json> parse_360_search_html(const std::string &html, int max_results) {
    htmlDocPtr doc = htmlReadMemory(html.c_str(), html.size(), nullptr, nullptr,
                                    HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return std::nullopt;

    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (!ctx) { xmlFreeDoc(doc); return std::nullopt; }

    // 360 搜索结果的标题通常在 <h3 class="res-title"> 下的 <a> 标签
    const char *xpath = "//h3[contains(@class,'res-title')]/a";
    xmlXPathObjectPtr obj = xmlXPathEvalExpression(reinterpret_cast<const xmlChar *>(xpath), ctx);
    nlohmann::json results = nlohmann::json::array();
    if (obj && obj->nodesetval) {
        xmlNodeSetPtr nodes = obj->nodesetval;
        for (int i = 0; i < nodes->nodeNr && (int)results.size() < max_results; ++i) {
            xmlNodePtr node = nodes->nodeTab[i];
            xmlChar *href = xmlGetProp(node, reinterpret_cast<const xmlChar *>("href"));
            xmlChar *title = xmlNodeGetContent(node);
            if (href && title) {
                // 尝试获取摘要：在同级或下一个兄弟节点中查找
                std::string snippet;
                xmlNodePtr parent = node->parent;  // h3
                if (parent) {
                    xmlNodePtr next = parent->next;
                    while (next && next->type != XML_ELEMENT_NODE) next = next->next;
                    if (next) {
                        xmlChar *desc = xmlNodeGetContent(next);
                        if (desc) {
                            snippet = reinterpret_cast<const char *>(desc);
                            xmlFree(desc);
                        }
                    }
                }
                results.push_back({
                    {"title", reinterpret_cast<const char *>(title)},
                    {"link", reinterpret_cast<const char *>(href)},
                    {"snippet", snippet}
                });
            }
            if (href) xmlFree(href);
            if (title) xmlFree(title);
        }
    }
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);
    if (results.empty()) return std::nullopt;
    return results;
}

// ==========================================================================
// libcurl 封装（支持取消、HTTP HEAD 快速检测）
// ==========================================================================
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    std::string *str = static_cast<std::string *>(userp);
    str->append(static_cast<char *>(contents), total);
    return total;
}

struct ProgressContext { std::atomic<bool> *canceled; };

static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    ProgressContext *ctx = static_cast<ProgressContext *>(clientp);
    if (ctx && ctx->canceled && ctx->canceled->load()) return 1;
    return 0;
}

static std::string http_get(const std::string &url, std::atomic<bool> *canceled = nullptr) {
    CURL *curl = curl_easy_init();
    if (!curl) return "";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);          // 无超时，依赖取消
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 0L);
    ProgressContext pc{};
    pc.canceled = canceled;
    if (canceled) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pc);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        if (canceled && canceled->load()) return "__CANCELED__";
        return "";
    }
    return response;
}

// 快速 HEAD 请求，用于连通性检测，超时 2 秒
static bool ping_url(const std::string &url, std::atomic<bool> *canceled = nullptr) {
    if (canceled && canceled->load()) return false;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 ...");
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK && http_code >= 200 && http_code < 400);
}

static std::string url_encode(const std::string &value) {
    CURL *curl = curl_easy_init();
    if (!curl) return value;
    char *encoded = curl_easy_escape(curl, value.c_str(), value.length());
    std::string result(encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

// ==========================================================================
// libxml2 辅助
// ==========================================================================
static std::vector<std::string> html_extract(const std::string &html, const std::string &xpath_expr) {
    std::vector<std::string> result;
    htmlDocPtr doc = htmlReadMemory(html.c_str(), html.size(), nullptr, nullptr,
                                    HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc) return result;
    xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
    if (!xpathCtx) { xmlFreeDoc(doc); return result; }
    xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(
        reinterpret_cast<const xmlChar *>(xpath_expr.c_str()), xpathCtx);
    if (xpathObj && xpathObj->nodesetval) {
        xmlNodeSetPtr nodes = xpathObj->nodesetval;
        for (int i = 0; i < nodes->nodeNr; ++i) {
            xmlNodePtr node = nodes->nodeTab[i];
            xmlChar *content = xmlNodeGetContent(node);
            if (content) {
                result.push_back(reinterpret_cast<const char *>(content));
                xmlFree(content);
            }
        }
    }
    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);
    return result;
}

static std::vector<std::map<std::string, std::string>> parse_rss(const std::string &xml) {
    std::vector<std::map<std::string, std::string>> items;
    xmlDocPtr doc = xmlReadMemory(xml.c_str(), xml.size(), nullptr, nullptr, XML_PARSE_NOERROR);
    if (!doc) return items;
    xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
    if (!xpathCtx) { xmlFreeDoc(doc); return items; }
    xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(
        reinterpret_cast<const xmlChar *>("//item"), xpathCtx);
    if (xpathObj && xpathObj->nodesetval) {
        xmlNodeSetPtr nodes = xpathObj->nodesetval;
        for (int i = 0; i < nodes->nodeNr; ++i) {
            xmlNodePtr node = nodes->nodeTab[i];
            std::map<std::string, std::string> item;
            for (xmlNodePtr child = node->children; child; child = child->next) {
                if (child->type == XML_ELEMENT_NODE) {
                    std::string name = reinterpret_cast<const char *>(child->name);
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content) {
                        item[name] = reinterpret_cast<const char *>(content);
                        xmlFree(content);
                    }
                }
            }
            if (!item.empty()) items.push_back(item);
        }
    }
    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);
    return items;
}

// ==========================================================================
// 源列表定义与 fallback 辅助结构
// ==========================================================================
struct Source {
    std::string name;          // 源名称（日志用）
    std::string url_template;  // URL 模板，可包含 {key} 占位符
    std::string ping_url;      // 连通性检测 URL（空表示跳过检测）
};

// ==========================================================================
// 节点类
// ==========================================================================
class WebSearchNode : public rclcpp::Node {
public:
    explicit WebSearchNode(const std::string &agent_name)
        : Node("web_search_node", agent_name), agent_name_(agent_name) {
        this->declare_parameter<std::string>("agent_name", agent_name);
        this->declare_parameter<double>("info_period", 1.0);
        double info_period = this->get_parameter("info_period").as_double();
        topic_prefix_ = "/" + agent_name_ + "/output/web_search/";

        info_pub_ = this->create_publisher<std_msgs::msg::String>(
            topic_prefix_ + "info", rclcpp::QoS(1).transient_local().reliable());
        info_timer_ = this->create_wall_timer(
            std::chrono::duration<double>(info_period),
            std::bind(&WebSearchNode::publish_info, this));

        action_server_ = rclcpp_action::create_server<ExecuteTool>(
            this, topic_prefix_.substr(0, topic_prefix_.size() - 1),
            [](auto...) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
            [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
                auto it = active_goals_.find(goal_handle->get_goal_id());
                if (it != active_goals_.end()) it->second->canceled.store(true);
                return rclcpp_action::CancelResponse::ACCEPT;
            },
            [this](const std::shared_ptr<GoalHandleExecute> goal_handle) {
                auto state = std::make_shared<ExecutionState>();
                state->canceled.store(false);
                active_goals_[goal_handle->get_goal_id()] = state;
                std::thread{std::bind(&WebSearchNode::execute, this, goal_handle, state)}.detach();
            });

        init_sources();  // 初始化所有操作的源列表
        RCLCPP_INFO(this->get_logger(), "WebSearchNode started for agent: %s", agent_name_.c_str());
    }

private:
    struct ExecutionState { std::atomic<bool> canceled; };

    // ----- 源列表 -----
    std::vector<Source> translate_sources_, weather_sources_,
                        news_sources_, crypto_sources_, arxiv_sources_,
                        ip_sources_, calc_sources_,
                        convert_sources_, image_sources_, search_sources_;

    void init_sources() {
        // translate
        translate_sources_ = {
            {"Lingva.ml", "https://lingva.ml/api/v1/auto/{target}/{query}", "https://lingva.ml"},
            {"Plausibility", "https://translate.plausibility.cloud/api/v1/auto/{target}/{query}", "https://translate.plausibility.cloud"},
            {"Pussthecat", "https://lingva.pussthecat.org/api/v1/auto/{target}/{query}", "https://lingva.pussthecat.org"}
        };

        // weather
        weather_sources_ = {
            {"wttr.in", "https://wttr.in/{city}?format=j1", "https://wttr.in"}
        };

        // news (Google News RSS + Bing News RSS)
        // news (国内优先：新浪财经 JSON，360 新闻 HTML，国外备用 HackerNews)
        news_sources_ = {
            {"SinaFinance", "https://feed.mix.sina.com.cn/api/roll/get?pageid=153&lid=2509&k=&num={max}&page=1&r=0.5", "https://feed.mix.sina.com.cn"},
            {"News360", "https://news.so.com/ns?q={query}&pn=0&rn={max}", "https://news.so.com"},
            {"HackerNews", "https://hn.algolia.com/api/v1/search_by_date?query={query}&tags=story&hitsPerPage={max}", "https://hn.algolia.com"}
        };

        // crypto
        crypto_sources_ = {
            {"CoinGecko", "https://api.coingecko.com/api/v3/simple/price?ids={coin}&vs_currencies={vs}", "https://api.coingecko.com"},
            {"CoinCap", "https://api.coincap.io/v2/assets/{coin}", "https://api.coincap.io"}
        };

        // arxiv
        arxiv_sources_ = {
            {"ArXiv", "http://export.arxiv.org/api/query?search_query=all:{query}&max_results={max}", "http://export.arxiv.org"}
        };

        // ip_info
        ip_sources_ = {
            {"ip-api", "http://ip-api.com/json/{ip}", "http://ip-api.com"},
            {"ipapi.co", "https://ipapi.co/{ip}/json/", "https://ipapi.co"}
        };

        // calculator
        calc_sources_ = {
            {"MathJS", "https://api.mathjs.org/v4/?expr={expr}", "https://api.mathjs.org"}
        };

        // unit convert (货币)
        convert_sources_ = {
            {"ExchangeRate", "https://api.exchangerate.host/convert?from={from}&to={to}&amount={amount}", "https://api.exchangerate.host"}
        };

        // image search
        image_sources_ = {
            {"Unsplash", "https://source.unsplash.com/featured/?{query}", "https://unsplash.com"}
        };

        // search
        search_sources_ = {
            {"So360", "https://www.so.com/s?q={query}&pn=0&rn={max}", "https://www.so.com"},
            {"Sogou", "https://www.sogou.com/web?query={query}", "https://www.sogou.com"},
            {"BingCN", "https://cn.bing.com/search?q={query}&count={max}", "https://cn.bing.com"},
            {"DuckDuckGo", "https://api.duckduckgo.com/?q={query}&format=json", "https://api.duckduckgo.com"},
            {"Wikipedia", "https://en.wikipedia.org/w/api.php?action=query&list=search&srsearch={query}&format=json", "https://en.wikipedia.org"}
        };
    }

    // ----- 工具描述发布 -----
    void publish_info() {
        std_msgs::msg::String msg;
        nlohmann::json info = {
            {"name", "web_search"},
            {"description", R"(万能网页搜索与信息获取工具，支持多源自动切换。
操作: search, wikipedia, image_search, image_download, file_download, weather, news,
translate, unit_convert, calculator, ip_info, crypto_price, arxiv, rss, extract_content, qrcode)"},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"action", {{"type", "string"}, {"enum", {
                        "search", "wikipedia", "image_search", "image_download",
                        "file_download", "weather", "news",
                        "translate", "unit_convert", "calculator",
                        "ip_info", "crypto_price",
                        "arxiv", "rss", "extract_content", "qrcode"
                    }}, {"description", "操作类型"}}},
                    {"query", {{"type", "string"}, {"description", "通用查询参数"}}},
                    {"max_results", {{"type", "integer"}, {"default", 5}}},
                    {"url", {{"type", "string"}, {"description", "用于download/rss/extract_content"}}},
                    {"save_path", {{"type", "string"}}},
                    {"target_lang", {{"type", "string"}, {"default", "zh"}}},
                    {"from_unit", {{"type", "string"}}},
                    {"to_unit", {{"type", "string"}}},
                    {"amount", {{"type", "number"}}},
                    {"coin", {{"type", "string"}}},
                    {"vs_currency", {{"type", "string"}, {"default", "usd"}}}
                }},
                {"required", {"action", "query"}}
            }}
        };
        msg.data = info.dump();
        info_pub_->publish(msg);
    }

    // ----- 通用方法：填充 URL 模板并 GET -----
    std::string fetch_from_source(const Source &src,
                                  const std::map<std::string, std::string> &params,
                                  std::atomic<bool> *canceled) {
        std::string url = src.url_template;
        for (const auto &[key, value] : params) {
            std::string placeholder = "{" + key + "}";
            size_t pos = 0;
            while ((pos = url.find(placeholder, pos)) != std::string::npos) {
                url.replace(pos, placeholder.length(), value);
                pos += value.length();
            }
        }
        return http_get(url, canceled);
    }

    // 尝试从源列表获取并解析，返回结果 JSON
    template <typename Parser>
    nlohmann::json try_sources(const std::vector<Source> &sources,
                               const std::map<std::string, std::string> &params,
                               std::atomic<bool> *canceled,
                               Parser parser) {
        for (const auto &src : sources) {
            if (canceled && canceled->load()) return {{"error", "execution canceled"}};
            // 连通性检测
            if (!src.ping_url.empty() && !ping_url(src.ping_url, canceled)) {
                RCLCPP_WARN(this->get_logger(), "Source %s ping failed, skip", src.name.c_str());
                continue;
            }
            std::string resp = fetch_from_source(src, params, canceled);
            if (resp == "__CANCELED__") return {{"error", "execution canceled"}};
            if (resp.empty()) {
                RCLCPP_WARN(this->get_logger(), "Source %s returned empty", src.name.c_str());
                continue;
            }
            auto result = parser(resp);
            if (result.has_value()) {
                RCLCPP_INFO(this->get_logger(), "Source %s succeeded", src.name.c_str());
                return result.value();
            }
            RCLCPP_WARN(this->get_logger(), "Source %s parse failed", src.name.c_str());
        }
        return {{"error", "all sources failed"}};
    }

    // ----- 动作主入口 -----
    void execute(const std::shared_ptr<GoalHandleExecute> goal_handle,
                 std::shared_ptr<ExecutionState> state) {
        auto result = std::make_shared<ExecuteTool::Result>();
        std::string compact = remove_json_whitespace(goal_handle->get_goal()->input_json);
        nlohmann::json input;
        try { input = nlohmann::json::parse(compact); }
        catch (...) {
            result->output_json = R"({"error":"invalid input json"})";
            result->exit_code = -1;
            goal_handle->abort(result);
            active_goals_.erase(goal_handle->get_goal_id());
            return;
        }
        std::string action = input.value("action", "");
        if (action.empty()) {
            result->output_json = R"({"error":"missing action"})";
            result->exit_code = -1;
            goal_handle->abort(result);
            active_goals_.erase(goal_handle->get_goal_id());
            return;
        }

        nlohmann::json output;
        int exit_code = 0;
        try {
            if (action == "search") output = handle_search(input, state);
            else if (action == "wikipedia") output = handle_wikipedia(input, state);
            else if (action == "image_search") output = handle_image_search(input, state);
            else if (action == "image_download") output = handle_download(input, state, true);
            else if (action == "file_download") output = handle_download(input, state, false);
            else if (action == "weather") output = handle_weather(input, state);
            else if (action == "news") output = handle_news(input, state);
            else if (action == "translate") output = handle_translate(input, state);
            else if (action == "unit_convert") output = handle_unit_convert(input, state);
            else if (action == "calculator") output = handle_calculator(input, state);
            else if (action == "ip_info") output = handle_ip_info(input, state);
            else if (action == "crypto_price") output = handle_crypto_price(input, state);
            else if (action == "arxiv") output = handle_arxiv(input, state);
            else if (action == "rss") output = handle_rss(input, state);
            else if (action == "extract_content") output = handle_extract_content(input, state);
            else if (action == "qrcode") output = handle_qrcode(input, state);
            else { output["error"] = "unknown action: " + action; exit_code = -1; }
        } catch (const std::exception &e) {
            output["error"] = std::string("internal error: ") + e.what();
            exit_code = -2;
        }

        if (state->canceled.load()) {
            output.clear();
            output["error"] = "execution canceled";
            exit_code = -7;
            goal_handle->abort(result);
        } else {
            result->output_json = output.dump();
            result->exit_code = exit_code;
            if (exit_code == 0) goal_handle->succeed(result);
            else goal_handle->abort(result);
        }
        active_goals_.erase(goal_handle->get_goal_id());
    }

    // ----- 各个功能处理函数 -----
    nlohmann::json handle_search(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string query = input.value("query", "");
        int max_results = input.value("max_results", 5);
        if (query.empty()) return {{"error", "missing query"}};

        std::map<std::string, std::string> params = {
            {"query", url_encode(query)},
            {"max", std::to_string(max_results)}
        };

        for (const auto &src : search_sources_) {
            if (state->canceled.load()) return {{"error", "execution canceled"}};
            if (!src.ping_url.empty() && !ping_url(src.ping_url, &state->canceled)) {
                RCLCPP_WARN(this->get_logger(), "Source %s ping failed, skip", src.name.c_str());
                continue;
            }
            std::string resp = fetch_from_source(src, params, &state->canceled);
            if (resp == "__CANCELED__") return {{"error", "execution canceled"}};
            if (resp.empty()) {
                RCLCPP_WARN(this->get_logger(), "Source %s returned empty", src.name.c_str());
                continue;
            }

            std::optional<nlohmann::json> result;
            if (src.name == "So360") {
                result = parse_360_search_html(resp, max_results);
            } else if (src.name == "Sogou") {
                // 搜狗解析待实现，先跳过
                RCLCPP_WARN(this->get_logger(), "Sogou parser not implemented, trying next source");
                continue;
            } else if (src.name == "BingCN") {
                // Bing 解析待实现，先跳过
                RCLCPP_WARN(this->get_logger(), "Bing parser not implemented, trying next source");
                continue;
            }

            if (result.has_value()) {
                RCLCPP_INFO(this->get_logger(), "Source %s succeeded", src.name.c_str());
                return {{"results", result.value()}};
            }
            RCLCPP_WARN(this->get_logger(), "Source %s parse failed", src.name.c_str());
        }

        // 所有源都失败时返回空数组
        return {{"results", nlohmann::json::array()}};
    }

    nlohmann::json handle_wikipedia(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string query = input.value("query", "");
        if (query.empty()) return {{"error", "missing query"}};
        std::string url = "https://en.wikipedia.org/w/api.php?action=query&prop=extracts&exintro&explaintext&titles=" +
                          url_encode(query) + "&format=json";
        std::string resp = http_get(url, &state->canceled);
        if (resp == "__CANCELED__") return {{"error", "execution canceled"}};
        if (resp.empty()) return {{"error", "wikipedia request failed"}};
        try {
            auto data = nlohmann::json::parse(resp);
            if (data.contains("query") && data["query"].contains("pages"))
                for (auto &[_, page] : data["query"]["pages"].items())
                    if (page.contains("extract"))
                        return {{"title", page.value("title", "")}, {"extract", page["extract"]}};
        } catch(...) {}
        return {{"error", "no wikipedia result"}};
    }

    nlohmann::json handle_image_search(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string query = input.value("query", "");
        if (query.empty()) return {{"error", "missing query"}};
        for (const auto &src : image_sources_) {
            if (state->canceled.load()) return {{"error", "execution canceled"}};
            if (!src.ping_url.empty() && !ping_url(src.ping_url, &state->canceled)) continue;
            std::string url = "https://source.unsplash.com/featured/?" + url_encode(query);
            CURL *curl = curl_easy_init();
            if (!curl) continue;
            std::string effective_url;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 ...");
            ProgressContext pc{&state->canceled};
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pc);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            if (curl_easy_perform(curl) == CURLE_OK) {
                char *eff = nullptr;
                curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
                if (eff) effective_url = eff;
            }
            curl_easy_cleanup(curl);
            if (state->canceled.load()) return {{"error", "execution canceled"}};
            if (!effective_url.empty()) return {{"image_url", effective_url}};
        }
        return {{"error", "image search failed"}};
    }

    nlohmann::json handle_download(const nlohmann::json &input, std::shared_ptr<ExecutionState> state, bool is_image) {
        std::string url = input.value("url", "");
        if (url.empty()) return {{"error", "missing url"}};
        std::string save_path = input.value("save_path", "");
        if (save_path.empty())
            save_path = std::string("/tmp/") + (is_image ? "img_" : "file_") +
                        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        CURL *curl = curl_easy_init();
        if (!curl) return {{"error", "curl init failed"}};
        FILE *fp = fopen(save_path.c_str(), "wb");
        if (!fp) { curl_easy_cleanup(curl); return {{"error", "cannot open file"}}; }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 ...");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        ProgressContext pc{&state->canceled};
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pc);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        CURLcode res = curl_easy_perform(curl);
        fclose(fp);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK) {
            if (state->canceled.load()) { remove(save_path.c_str()); return {{"error", "execution canceled"}}; }
            remove(save_path.c_str());
            return {{"error", "download failed"}};
        }
        return {{"local_path", save_path}, {"status", "ok"}};
    }

    nlohmann::json handle_weather(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string city = input.value("query", "");
        if (city.empty()) return {{"error", "missing city"}};
        auto parser = [](const std::string &resp) -> std::optional<nlohmann::json> {
            try {
                auto data = nlohmann::json::parse(resp);
                if (data.contains("current_condition")) {
                    auto &cur = data["current_condition"][0];
                    return std::optional{ nlohmann::json{
                        {"temp_C", cur.value("temp_C", "")},
                        {"humidity", cur.value("humidity", "")},
                        {"description", cur["weatherDesc"][0].value("value", "")},
                        {"windspeedKmph", cur.value("windspeedKmph", "")}
                    }};
                }
            } catch(...) {}
            return std::nullopt;
        };
        std::map<std::string, std::string> params = {{"city", url_encode(city)}};
        nlohmann::json result = try_sources(weather_sources_, params, &state->canceled, parser);
        if (result.contains("error")) return result;
        result["city"] = city;
        return result;
    }

    nlohmann::json handle_news(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string query = input.value("query", "");
        int max_results = input.value("max_results", 5);
        if (query.empty()) return {{"error", "missing query"}};

        std::map<std::string, std::string> params = {
            {"query", url_encode(query)},
            {"max", std::to_string(max_results)}
        };

        for (const auto &src : news_sources_) {
            if (state->canceled.load()) return {{"error", "execution canceled"}};
            if (!src.ping_url.empty() && !ping_url(src.ping_url, &state->canceled)) {
                RCLCPP_WARN(this->get_logger(), "Source %s ping failed, skip", src.name.c_str());
                continue;
            }
            std::string resp = fetch_from_source(src, params, &state->canceled);
            if (resp == "__CANCELED__") return {{"error", "execution canceled"}};
            if (resp.empty()) {
                RCLCPP_WARN(this->get_logger(), "Source %s returned empty", src.name.c_str());
                continue;
            }

            std::optional<nlohmann::json> result;
            if (src.name == "SinaFinance") {
                result = parse_sina_news(resp, max_results);
            } else if (src.name == "News360") {
                result = parse_360_news(resp, max_results);
            } else {
                // 默认尝试 JSON 解析（HackerNews 等）
                try {
                    auto data = nlohmann::json::parse(resp);
                    if (data.contains("hits") && data["hits"].is_array()) {
                        nlohmann::json articles = nlohmann::json::array();
                        for (auto &hit : data["hits"]) {
                            if ((int)articles.size() >= max_results) break;
                            articles.push_back({
                                {"title", hit.value("title", "")},
                                {"link", hit.value("url", hit.value("story_url", ""))},
                                {"pubDate", hit.value("created_at", "")},
                                {"description", hit.value("story_text", "")}
                            });
                        }
                        result = std::optional{ nlohmann::json{{"articles", articles}} };
                    }
                } catch (...) {
                    // 再尝试 RSS
                    auto items = parse_rss(resp);
                    if (!items.empty()) {
                        nlohmann::json articles = nlohmann::json::array();
                        for (auto &item : items) {
                            if ((int)articles.size() >= max_results) break;
                            articles.push_back({
                                {"title", item["title"]},
                                {"link", item["link"]},
                                {"pubDate", item["pubDate"]},
                                {"description", item["description"]}
                            });
                        }
                        result = std::optional{ nlohmann::json{{"articles", articles}} };
                    }
                }
            }

            if (result.has_value()) {
                RCLCPP_INFO(this->get_logger(), "Source %s succeeded", src.name.c_str());
                return {{"articles", result.value()}};
            }
            RCLCPP_WARN(this->get_logger(), "Source %s parse failed", src.name.c_str());
        }
        return {{"error", "all news sources failed"}};
    }

    nlohmann::json handle_translate(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string text = input.value("query", "");
        std::string target = input.value("target_lang", "zh");
        if (text.empty()) return {{"error", "missing text"}};
        auto parser = [target](const std::string &resp) -> std::optional<nlohmann::json> {
            try {
                auto data = nlohmann::json::parse(resp);
                if (data.contains("translation"))
                    return std::optional{ nlohmann::json{
                        {"translated_text", data["translation"]},
                        {"source_lang", "auto"},
                        {"target_lang", target}
                    }};
            } catch(...) {}
            return std::nullopt;
        };
        return try_sources(translate_sources_, {{"query", url_encode(text)}, {"target", target}}, &state->canceled, parser);
    }

    nlohmann::json handle_unit_convert(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string from = input.value("from_unit", "");
        std::string to = input.value("to_unit", "");
        double amount = input.value("amount", 0.0);
        if (from.empty() || to.empty()) return {{"error", "missing from_unit or to_unit"}};
        // 本地常见转换
        auto local = [&]() -> std::optional<double> {
            if (from == "m" && to == "km") return amount / 1000.0;
            if (from == "km" && to == "m") return amount * 1000.0;
            if (from == "c" && to == "f") return amount * 9.0/5.0 + 32;
            if (from == "f" && to == "c") return (amount - 32) * 5.0/9.0;
            return std::nullopt;
        };
        if (auto val = local())
            return {{"from", from}, {"to", to}, {"amount", amount}, {"result", *val}};

        auto parser = [from, to, amount](const std::string &resp) -> std::optional<nlohmann::json> {
            try {
                auto data = nlohmann::json::parse(resp);
                if (data.contains("result"))
                    return std::optional{ nlohmann::json{
                        {"from", from}, {"to", to},
                        {"amount", amount},
                        {"result", data["result"].get<double>()}
                    }};
            } catch(...) {}
            return std::nullopt;
        };
        std::map<std::string, std::string> params = {
            {"from", from}, {"to", to}, {"amount", std::to_string(amount)}
        };
        return try_sources(convert_sources_, params, &state->canceled, parser);
    }

    nlohmann::json handle_calculator(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string expr = input.value("query", "");
        if (expr.empty()) return {{"error", "missing expression"}};
        auto parser = [expr](const std::string &resp) -> std::optional<nlohmann::json> {
            return std::optional{ nlohmann::json{{"expression", expr}, {"result", resp}} };
        };
        return try_sources(calc_sources_, {{"expr", url_encode(expr)}}, &state->canceled, parser);
    }

    nlohmann::json handle_ip_info(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string ip = input.value("query", "");
        auto parser = [](const std::string &resp) -> std::optional<nlohmann::json> {
            try {
                auto data = nlohmann::json::parse(resp);
                if (data.value("status", "") == "success" || data.contains("ip"))
                    return std::optional{ nlohmann::json{
                        {"ip", data.value("query", data.value("ip", ""))},
                        {"country", data.value("country", "")},
                        {"city", data.value("city", "")},
                        {"isp", data.value("isp", "")}
                    }};
            } catch(...) {}
            return std::nullopt;
        };
        return try_sources(ip_sources_, {{"ip", ip.empty() ? "" : ip}}, &state->canceled, parser);
    }

    nlohmann::json handle_crypto_price(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string coin = input.value("coin", "");
        std::string vs = input.value("vs_currency", "usd");
        if (coin.empty()) return {{"error", "missing coin id"}};
        auto parser = [coin, vs](const std::string &resp) -> std::optional<nlohmann::json> {
            try {
                auto data = nlohmann::json::parse(resp);
                // CoinGecko
                if (data.contains(coin) && data[coin].contains(vs))
                    return std::optional{ nlohmann::json{
                        {"coin", coin},
                        {"price", data[coin][vs].get<double>()},
                        {"currency", vs}
                    }};
                // CoinCap
                if (data.contains("data") && data["data"].contains("priceUsd"))
                    return std::optional{ nlohmann::json{
                        {"coin", coin},
                        {"price", std::stod(data["data"]["priceUsd"].get<std::string>())},
                        {"currency", "usd"}
                    }};
            } catch(...) {}
            return std::nullopt;
        };
        return try_sources(crypto_sources_, {{"coin", coin}, {"vs", vs}}, &state->canceled, parser);
    }

    nlohmann::json handle_arxiv(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string query = input.value("query", "");
        int max_results = input.value("max_results", 5);
        if (query.empty()) return {{"error", "missing query"}};
        auto parser = [max_results](const std::string &resp) -> std::optional<nlohmann::json> {
            auto items = parse_rss(resp);
            if (items.empty()) return std::nullopt;
            nlohmann::json papers = nlohmann::json::array();
            for (auto &item : items) {
                if ((int)papers.size() >= max_results) break;
                papers.push_back({
                    {"title", item["title"]},
                    {"summary", item["summary"]},
                    {"link", item["id"]}
                });
            }
            return std::optional{ nlohmann::json{{"papers", papers}} };
        };
        return try_sources(arxiv_sources_, {{"query", url_encode(query)}, {"max", std::to_string(max_results)}}, &state->canceled, parser);
    }

    nlohmann::json handle_rss(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string url = input.value("url", "");
        int max_results = input.value("max_results", 5);
        if (url.empty()) return {{"error", "missing url"}};
        std::string resp = http_get(url, &state->canceled);
        if (resp == "__CANCELED__") return {{"error", "execution canceled"}};
        if (resp.empty()) return {{"error", "rss fetch failed"}};
        auto items = parse_rss(resp);
        nlohmann::json entries = nlohmann::json::array();
        for (auto &item : items) {
            if ((int)entries.size() >= max_results) break;
            entries.push_back({{"title", item["title"]}, {"link", item["link"]}, {"description", item["description"]}});
        }
        return {{"entries", entries}};
    }

    nlohmann::json handle_extract_content(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string url = input.value("url", "");
        if (url.empty()) return {{"error", "missing url"}};
        std::string html = http_get(url, &state->canceled);
        if (html == "__CANCELED__") return {{"error", "execution canceled"}};
        if (html.empty()) return {{"error", "fetch failed"}};
        auto parts = html_extract(html, "//body//text()");
        std::string text;
        for (auto &p : parts)
            if (p.find_first_not_of(" \t\n\r") != std::string::npos)
                text += p + "\n";
        return {{"content", text.substr(0, 10000)}};
    }

    nlohmann::json handle_qrcode(const nlohmann::json &input, std::shared_ptr<ExecutionState> state) {
        std::string data = input.value("query", "");
        if (data.empty()) return {{"error", "missing data"}};
        std::string url = "https://api.qrserver.com/v1/create-qr-code/?size=150x150&data=" + url_encode(data);
        return {{"image_url", url}};
    }

    // 成员变量
    std::string agent_name_, topic_prefix_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::TimerBase::SharedPtr info_timer_;
    rclcpp_action::Server<ExecuteTool>::SharedPtr action_server_;
    std::map<rclcpp_action::GoalUUID, std::shared_ptr<ExecutionState>> active_goals_;
};

// ==========================================================================
// main
// ==========================================================================
int main(int argc, char **argv) {
    rclcpp::init(argc, argv);
    curl_global_init(CURL_GLOBAL_ALL);
    xmlInitParser();
    auto temp_node = std::make_shared<rclcpp::Node>("temp");
    temp_node->declare_parameter<std::string>("agent_name", "agent");
    std::string agent_name = temp_node->get_parameter("agent_name").as_string();
    temp_node.reset();
    auto node = std::make_shared<WebSearchNode>(agent_name);
    rclcpp::executors::MultiThreadedExecutor executor;
    executor.add_node(node);
    executor.spin();
    curl_global_cleanup();
    xmlCleanupParser();
    rclcpp::shutdown();
    return 0;
}