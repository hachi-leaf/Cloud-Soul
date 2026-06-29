// Copyright (c) leaf
// SPDX-License-Identifier: MIT
// Interactive command-line chat tool using OpenAIClient

#include "cs_core/call_openai.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <map>
#include <functional>

using namespace openai_client;

// ---------- color helpers ----------
#define COLOR_RESET   "\033[0m"
#define COLOR_USER    "\033[32m"   // green
#define COLOR_THINK   "\033[33m"   // yellow
#define COLOR_REPLY   "\033[37m"   // white
#define COLOR_TOOL    "\033[34m"   // blue
#define COLOR_SYSTEM  "\033[35m"   // magenta
#define COLOR_CMD     "\033[36m"   // cyan

// ---------- globals ----------
static std::atomic<bool> g_interrupted{false};
static OpenAIClient*    g_client = nullptr;

extern "C" void signal_handler(int) {
    g_interrupted.store(true);
    if (g_client) {
        g_client->cancel_request();
    }
}

// ---------- mock tools ----------
nlohmann::json get_date_mock(const nlohmann::json& /*args*/) {
    time_t now = time(nullptr);
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", localtime(&now));
    return {{"date", buf}};
}

nlohmann::json get_weather_mock(const nlohmann::json& args) {
    std::string location = args.value("location", "unknown");
    std::string date = args.value("date", "today");
    int hash = std::hash<std::string>{}(location + date);
    const char* conditions[] = {"Sunny", "Cloudy", "Rainy", "Snowy"};
    std::string cond = conditions[hash % 4];
    int temp = 10 + (hash % 20);  // 10~29°C
    return {{"condition", cond}, {"temperature", std::to_string(temp) + "°C"}};
}

const nlohmann::json tools_def = nlohmann::json::array({
    {
        {"type", "function"},
        {"function", {
            {"name", "get_date"},
            {"description", "Get the current date"},
            {"parameters", {{"type", "object"}, {"properties", nlohmann::json::object()}}}
        }}
    },
    {
        {"type", "function"},
        {"function", {
            {"name", "get_weather"},
            {"description", "Get weather of a location, the user should supply the location and date."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"location", {{"type", "string"}, {"description", "The city name"}}},
                    {"date", {{"type", "string"}, {"description", "The date in format YYYY-mm-dd"}}}
                }},
                {"required", {"location", "date"}}
            }}
        }}
    }
});

std::string execute_tool(const std::string& name, const nlohmann::json& args) {
    if (name == "get_date") return get_date_mock(args).dump();
    if (name == "get_weather") return get_weather_mock(args).dump();
    return "{}";
}

// ---------- command parser ----------
void print_help() {
    std::cout << COLOR_CMD
              << "Available commands:\n"
              << "  /model <name>          - change model\n"
              << "  /temperature <float>   - set temperature\n"
              << "  /top_p <float>         - set top_p\n"
              << "  /max_tokens <int>      - set max tokens\n"
              << "  /thinking on|off       - toggle thinking\n"
              << "  /effort <level>        - reasoning effort (high/medium/low/max)\n"
              << "  /clear                 - clear message history\n"
              << "  /exit                  - exit program\n"
              << "  /help                  - this message\n"
              << COLOR_RESET;
}

bool handle_command(const std::string& input) {
    std::istringstream iss(input);
    std::string cmd;
    iss >> cmd;

    if (cmd == "/exit") {
        return false;
    } else if (cmd == "/help") {
        print_help();
    } else if (cmd == "/clear") {
        g_client->clear_messages();
        std::cout << COLOR_SYSTEM << "[system] Message history cleared.\n" << COLOR_RESET;
    } else if (cmd == "/model") {
        std::string model;
        if (iss >> model) {
            g_client->set_model(model);
            std::cout << COLOR_SYSTEM << "[system] Model set to " << model << "\n" << COLOR_RESET;
        } else {
            std::cout << "Usage: /model <model_name>\n";
        }
    } else if (cmd == "/temperature") {
        double temp;
        if (iss >> temp) {
            g_client->set_temperature(temp);
            std::cout << COLOR_SYSTEM << "[system] Temperature set to " << temp << "\n" << COLOR_RESET;
        } else {
            std::cout << "Usage: /temperature <float>\n";
        }
    } else if (cmd == "/top_p") {
        double tp;
        if (iss >> tp) {
            g_client->set_top_p(tp);
            std::cout << COLOR_SYSTEM << "[system] Top_p set to " << tp << "\n" << COLOR_RESET;
        } else {
            std::cout << "Usage: /top_p <float>\n";
        }
    } else if (cmd == "/max_tokens") {
        int mt;
        if (iss >> mt) {
            g_client->set_max_tokens(mt);
            std::cout << COLOR_SYSTEM << "[system] Max_tokens set to " << mt << "\n" << COLOR_RESET;
        } else {
            std::cout << "Usage: /max_tokens <int>\n";
        }
    } else if (cmd == "/thinking") {
        std::string val;
        if (iss >> val) {
            if (val == "on") {
                g_client->set_thinking_enabled(true);
                std::cout << COLOR_SYSTEM << "[system] Thinking enabled.\n" << COLOR_RESET;
            } else if (val == "off") {
                g_client->set_thinking_enabled(false);
                std::cout << COLOR_SYSTEM << "[system] Thinking disabled.\n" << COLOR_RESET;
            } else {
                std::cout << "Usage: /thinking on|off\n";
            }
        } else {
            std::cout << "Usage: /thinking on|off\n";
        }
    } else if (cmd == "/effort") {
        std::string effort;
        if (iss >> effort) {
            g_client->set_reasoning_effort(effort);
            std::cout << COLOR_SYSTEM << "[system] Reasoning effort set to " << effort << "\n" << COLOR_RESET;
        } else {
            std::cout << "Usage: /effort <high|medium|low|max>\n";
        }
    } else {
        std::cout << "Unknown command: " << cmd << "\n";
    }
    return true;
}

// ---------- main chat logic ----------
void process_turn(const std::string& user_input) {
    g_client->add_message({{"role", "user"}, {"content", user_input}});

    // 用共享状态把 think 和 reply 区块分开，流式输出时不会糊在一起
    enum class StreamPhase { THINK, REPLY, NONE };
    auto phase = std::make_shared<StreamPhase>(StreamPhase::NONE);

    auto think_cb = [phase](const std::string& chunk) {
        if (*phase != StreamPhase::THINK) {
            if (*phase == StreamPhase::REPLY) std::cout << "\n";
            std::cout << COLOR_THINK << "[think]\n--------------------------------------------------\n";
            *phase = StreamPhase::THINK;
        }
        std::cout << chunk;
    };
    auto reply_cb = [phase](const std::string& chunk) {
        if (*phase != StreamPhase::REPLY) {
            if (*phase == StreamPhase::THINK) std::cout << COLOR_RESET << "\n";
            std::cout << COLOR_REPLY << "[reply]\n--------------------------------------------------\n";
            *phase = StreamPhase::REPLY;
        }
        std::cout << chunk;
    };

    g_client->set_think_callback(think_cb);
    g_client->set_reply_callback(reply_cb);

    // 循环处理可能的工具调用
    while (true) {
        g_interrupted.store(false);
        nlohmann::json assistant_msg;
        try {
            assistant_msg = g_client->call_api(true, tools_def);
        } catch (const std::exception& e) {
            // 如果是用户主动中断，不要当成 API 错误喷红字
            if (g_interrupted.load()) {
                g_client->add_message({{"role", "system"}, {"content", "用户打断了对话"}});
                std::cout << COLOR_SYSTEM << "\n[system] Interrupted. Partial reply saved.\n" << COLOR_RESET;
                return;
            }
            std::cerr << COLOR_SYSTEM << "\n[system] API error: " << e.what() << COLOR_RESET << "\n";
            return;
        }

        std::cout << COLOR_RESET << "\n";

        if (g_interrupted.load()) {
            // 走到这里说明 call_api 正常返回了但中途收到过信号，
            // 把已经拿到的不完整回复和一条打断提示加进历史
            g_client->add_message(assistant_msg);
            g_client->add_message({{"role", "system"}, {"content", "用户打断了对话"}});
            std::cout << COLOR_SYSTEM << "[system] Interrupted. Partial reply saved.\n" << COLOR_RESET;
            return;
        }

        // 没有中断，正常处理完整的 assistant 消息
        nlohmann::json msg_to_store = assistant_msg;
        if (!assistant_msg.contains("tool_calls") || assistant_msg["tool_calls"].is_null()) {
            msg_to_store.erase("reasoning_content");
        }
        g_client->add_message(msg_to_store);

        // 看看要不要调工具
        if (assistant_msg.contains("tool_calls") && !assistant_msg["tool_calls"].is_null() &&
            !assistant_msg["tool_calls"].empty()) {
            auto tool_calls = assistant_msg["tool_calls"];
            for (const auto& tc : tool_calls) {
                std::string tool_id = tc.value("id", "");
                std::string func_name = tc.value("function", nlohmann::json())["name"];
                std::string func_args_str = tc.value("function", nlohmann::json())["arguments"];

                std::cout << COLOR_TOOL << "[tool] calling " << func_name 
                          << " with args: " << func_args_str << COLOR_RESET << "\n";

                nlohmann::json args = nlohmann::json::parse(func_args_str.empty() ? "{}" : func_args_str);
                std::string result = execute_tool(func_name, args);
                std::cout << COLOR_TOOL << "[tool] result: " << result << COLOR_RESET << "\n";

                g_client->add_message({
                    {"role", "tool"},
                    {"tool_call_id", tool_id},
                    {"content", result}
                });
            }
            continue;  // 继续调 API
        } else {
            break;     // 这次对话结束
        }
    }
}

int main() {
    std::cout << std::unitbuf;

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    const char* api_url = std::getenv("OPENAI_API_URL");
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_url || !api_key) {
        std::cerr << "Please set OPENAI_API_URL and OPENAI_API_KEY environment variables.\n";
        return 1;
    }

    OpenAIClient client(api_url, api_key, "HORIZON-CO-4.7");
    g_client = &client;

    g_client->add_message({
        {"role", "system"},
        {"content", "你是一个AI对话助手，满足用户的所有要求。"}
    });

    client.set_thinking_enabled(true);
    client.set_reasoning_effort("high");

    std::cout << COLOR_SYSTEM << "Chat started. Type /help for commands, Ctrl+C to interrupt.\n"
              << COLOR_RESET;

    std::string line;
    while (true) {
        std::cout << COLOR_USER << "\nYou> " << COLOR_RESET;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line[0] == '/') {
            if (!handle_command(line)) break;
            continue;
        }

        process_turn(line);
    }

    std::cout << "\nGoodbye.\n";
    return 0;
}