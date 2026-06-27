// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// Cloud-Soul 标准 input 源节点：系统状态采集
// unified system status collector for Cloud-Soul
//
// Node: /<agent_name>/system_status_node
//
// 作用:
//   周期性采集 CPU 使用率、内存占用、磁盘空间、网络接口、UTC 时间、
//   GPU 名称、主机名、当前用户、机器码 (machine-id)。
//   以 JSON 格式通过 info 与 data 话题对外发布。
//   info 话题每次心跳均发布，提供固定描述、采集模式与健康状态。
//   data 话题每次采集均发布，始终包含最新时间戳。
//
// 参数:
//   <string>agent_name    - Agent 命名空间，默认 "agent"
//   <float64>publish_rate - 采集及 info 发布频率（Hz），默认 1.0
//
// 对外接口（遵循 input 模块规范）：
//   话题 /<agent_name>/input/system_status/info  (std_msgs/String, JSON)
//     内容: {"desc":"系统状态","mode":"latest","status":"ok"} 或
//           {"desc":"系统状态","mode":"lastest","status":"partial_failure"}
//     QoS: transient_local + reliable, depth=1
//
//   话题 /<agent_name>/input/system_status       (std_msgs/String, JSON)
//     内容: 结构化系统状态 JSON，每次采集均发布，包含实时时间戳。示例:
//     {
//       "cpu": 12,
//       "mem": {"used_mb": 1024, "total_mb": 4096},
//       "disk": {"mount": "/", "used_gb": 8, "total_gb": 50},
//       "net": [{"name": "eth0", "ip": "192.168.1.2"}],
//       "time": "2025-01-01T00:00:00Z",
//       "gpu": "NVIDIA GeForce RTX 3060",
//       "host": "myhost",
//       "user": "leaf",
//       "machine_id": "abc123def456..."
//     }
//     字段失败时为 null（例如 "cpu": null）。
//     QoS: reliable + transient_local, depth=1
//
// 行为特性:
//   1. 周期性采集：定时器按 publish_rate 触发，每次采集并发布 info 和 data。
//   2. 无变化抑制：data 话题每次 tick 均发布，确保时间戳实时更新。
//   3. 健康状态：若任一采集项失败（异常），info 的 status 置为 "partial_failure"，
//      否则为 "ok"。失败字段在 data 中设为 null，不影响其他字段采集。
//   4. 超时控制：调用 nvidia-smi 使用 popen + select 实现 3 秒超时，
//      超时后 GPU 字段置 null 并设置 status 为 "partial_failure"。
//   5. 优雅退出：收到 shutdown 信号后，通过 shutting_down_ 标志停止定时器，
//      tick 回调立即返回，避免退出期间继续发布。
//   6. 参数防御：publish_rate ≤ 0 时使用默认 1.0 并输出警告。
//   7. 所有系统调用均包裹 try‑catch，异常不扩散，保证节点持续运行。
//   8. 无外部依赖自定义消息，全部使用 std_msgs/String + nlohmann::json。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <exception>
#include <signal.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "nlohmann/json.hpp"

using namespace std::chrono_literals;
using json = nlohmann::json;

// ================================================================
// Info 模板（固定 desc + mode）
// ================================================================
static constexpr const char* SYSTEM_STATUS_INFO_TEMPLATE = R"json({
  "desc": "系统状态",
  "mode": "latest"
})json";

// ================================================================
// 超时控制工具（popen 超时）
// ================================================================
static std::string exec_cmd_with_timeout(const char* cmd, int timeout_sec = 3) {
    std::string result;
    FILE* fp = popen(cmd, "r");
    if (!fp) return result;

    int fd = fileno(fp);
    fd_set set;
    struct timeval timeout;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;

    int ret = select(fd + 1, &set, nullptr, nullptr, &timeout);
    if (ret > 0) {
        char buf[256];
        if (fgets(buf, sizeof(buf), fp) != nullptr) {
            result = buf;
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
    } else {
        RCLCPP_WARN(rclcpp::get_logger("system_status"), "Command timeout: %s", cmd);
    }
    pclose(fp);
    return result;
}

// ================================================================
// CPU 采集（/proc/stat 瞬时使用率）
// ================================================================
static double get_cpu_usage() {
    static unsigned long long prev_idle = 0, prev_total = 0;
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) throw std::runtime_error("cannot open /proc/stat");
    std::string line;
    std::getline(stat, line);
    stat.close();

    char cpu[4];
    unsigned long long user, nice, system, idle, iowait, irq, softirq;
    if (sscanf(line.c_str(), "%s %llu %llu %llu %llu %llu %llu %llu",
               cpu, &user, &nice, &system, &idle, &iowait, &irq, &softirq) < 8)
        throw std::runtime_error("malformed /proc/stat");

    unsigned long long total = user + nice + system + idle + iowait + irq + softirq;
    unsigned long long total_diff = total - prev_total;
    unsigned long long idle_diff  = idle - prev_idle;
    prev_total = total;
    prev_idle  = idle;
    if (total_diff == 0) return 0.0;
    return 100.0 * (total_diff - idle_diff) / total_diff;
}

// ================================================================
// 节点主体
// ================================================================
class SystemStatusNode : public rclcpp::Node {
public:
    explicit SystemStatusNode(const std::string& agent_name)
        : Node("system_status_node", agent_name), agent_name_(agent_name), shutting_down_(false)
    {
        this->declare_parameter<std::string>("agent_name", agent_name);
        this->declare_parameter<double>("publish_rate", 1.0);

        double rate = this->get_parameter("publish_rate").as_double();
        if (rate <= 0.0) {
            RCLCPP_WARN(this->get_logger(), "Invalid publish_rate %.2f, using 1.0", rate);
            rate = 1.0;
        }

        std::string ns = "/" + agent_name_;

        // ---- 发布器 ----
        rclcpp::QoS info_qos(1);
        info_qos.transient_local();
        info_qos.reliable();
        info_pub_ = this->create_publisher<std_msgs::msg::String>(
            ns + "/input/system_status/info", info_qos);

        data_pub_ = this->create_publisher<std_msgs::msg::String>(
            ns + "/input/system_status",
            rclcpp::QoS(1).reliable().transient_local());

        // ---- 定时采集 ----
        timer_ = this->create_wall_timer(
            std::chrono::duration<double>(1.0 / rate),
            std::bind(&SystemStatusNode::tick, this));

        // 立即发布一次 info
        publish_info("ok");
        RCLCPP_INFO(this->get_logger(), "SystemStatusNode ready (agent=%s)", agent_name_.c_str());
    }

    ~SystemStatusNode() {
        on_shutdown();
    }

    void on_shutdown() {
        if (shutting_down_.exchange(true)) return;
        if (timer_) {
            timer_->cancel();
        }
        RCLCPP_INFO(this->get_logger(), "SystemStatusNode shutting down");
    }

private:
    // ---------------------------------------------------------------
    // 发布 info 话题
    // ---------------------------------------------------------------
    void publish_info(const std::string& status) {
        json info = json::parse(SYSTEM_STATUS_INFO_TEMPLATE);
        info["status"] = status;
        auto msg = std_msgs::msg::String();
        try {
            msg.data = info.dump();
            info_pub_->publish(msg);
        } catch (const std::exception& e) {
            RCLCPP_ERROR(this->get_logger(), "Failed to publish info: %s", e.what());
        }
    }

    // ---------------------------------------------------------------
    // 定时器回调：采集并发布
    // ---------------------------------------------------------------
    void tick() {
        if (shutting_down_) return;

        std::string data_json = build_status_json();
        if (data_json.empty()) {
            RCLCPP_ERROR(this->get_logger(), "build_status_json returned empty");
            return;
        }

        publish_info(overall_status_);

        auto msg = std_msgs::msg::String();
        msg.data = data_json;
        try {
            data_pub_->publish(msg);
        } catch (const std::exception& e) {
            RCLCPP_ERROR(this->get_logger(), "Failed to publish data: %s", e.what());
        }
    }

    // ---------------------------------------------------------------
    // 构建完整系统状态 JSON
    // ---------------------------------------------------------------
    std::string build_status_json() {
        json j;
        bool any_failure = false;

        // ---- CPU ----
        try {
            double cpu = get_cpu_usage();
            j["cpu"] = static_cast<int>(cpu);
        } catch (...) {
            j["cpu"] = nullptr;
            any_failure = true;
        }

        // ---- 内存 ----
        try {
            struct sysinfo si;
            if (sysinfo(&si) == 0) {
                unsigned long total_mb = si.totalram * si.mem_unit / (1024 * 1024);
                unsigned long free_mb  = si.freeram  * si.mem_unit / (1024 * 1024);
                j["mem"]["used_mb"]  = total_mb - free_mb;
                j["mem"]["total_mb"] = total_mb;
            } else {
                throw std::runtime_error("sysinfo failed");
            }
        } catch (...) {
            j["mem"] = nullptr;
            any_failure = true;
        }

        // ---- 磁盘 / ----
        try {
            struct statvfs sv;
            if (statvfs("/", &sv) == 0) {
                unsigned long long total_gb = sv.f_blocks * sv.f_frsize / (1024LL*1024*1024);
                unsigned long long avail_gb = sv.f_bavail * sv.f_frsize / (1024LL*1024*1024);
                j["disk"]["mount"] = "/";
                j["disk"]["used_gb"] = total_gb - avail_gb;
                j["disk"]["total_gb"] = total_gb;
            } else {
                throw std::runtime_error("statvfs failed");
            }
        } catch (...) {
            j["disk"] = nullptr;
            any_failure = true;
        }

        // ---- 网络 ----
        try {
            struct ifaddrs *ifaddr, *ifa;
            if (getifaddrs(&ifaddr) == 0) {
                json net_arr = json::array();
                for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
                    if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                        std::string ifname(ifa->ifa_name);
                        if (ifname == "lo") continue;
                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET,
                                  &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr,
                                  ip, sizeof(ip));
                        net_arr.push_back({{"name", ifname}, {"ip", ip}});
                    }
                }
                freeifaddrs(ifaddr);
                j["net"] = net_arr;
            } else {
                throw std::runtime_error("getifaddrs failed");
            }
        } catch (...) {
            j["net"] = nullptr;
            any_failure = true;
        }

        // ---- UTC 时间 ----
        std::time_t now = std::time(nullptr);
        std::tm* gmt = std::gmtime(&now);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmt);
        j["time"] = time_buf;

        // ---- GPU ----
        try {
            std::string gpu = exec_cmd_with_timeout(
                "nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null", 3);
            j["gpu"] = gpu.empty() ? json(nullptr) : json(gpu);
        } catch (...) {
            j["gpu"] = nullptr;
            any_failure = true;
        }

        // ---- 主机名 ----
        try {
            char hostname[256];
            if (gethostname(hostname, sizeof(hostname)) == 0) {
                j["host"] = hostname;
            } else {
                throw std::runtime_error("gethostname failed");
            }
        } catch (...) {
            j["host"] = nullptr;
            any_failure = true;
        }

        // ---- 用户 ----
        const char* user = std::getenv("USER");
        j["user"] = user ? user : "unknown";

        // ---- machine-id ----
        try {
            std::ifstream mid_file("/etc/machine-id");
            std::string mid;
            if (mid_file.is_open()) {
                std::getline(mid_file, mid);
                mid_file.close();
                if (!mid.empty()) {
                    mid.erase(mid.find_last_not_of(" \t\n\r") + 1);
                    j["machine_id"] = mid;
                } else {
                    j["machine_id"] = nullptr;
                }
            } else {
                j["machine_id"] = nullptr;
            }
        } catch (...) {
            j["machine_id"] = nullptr;
            any_failure = true;
        }

        overall_status_ = any_failure ? "partial_failure" : "ok";
        return j.dump();
    }

    // 成员变量
    std::string agent_name_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr info_pub_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr data_pub_;
    rclcpp::TimerBase::SharedPtr timer_;
    std::atomic<bool> shutting_down_{false};
    std::string overall_status_{"ok"};
};

// ================================================================
// main
// ================================================================
int main(int argc, char** argv) {
    rclcpp::init(argc, argv);

    // 临时节点读取参数
    auto temp = std::make_shared<rclcpp::Node>("temp");
    temp->declare_parameter<std::string>("agent_name", "agent");
    std::string agent_name = temp->get_parameter("agent_name").as_string();
    temp.reset();

    auto node = std::make_shared<SystemStatusNode>(agent_name);
    rclcpp::on_shutdown([&node]() { node->on_shutdown(); });
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}