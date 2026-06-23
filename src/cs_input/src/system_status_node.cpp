// Copyright (c) 2026 Leaf
// SPDX-License-Identifier: MIT

// 节点: /<agent_name>/system_status_node (输入源节点，由 input_mgmt_node 自动发现)
// 作用: 周期性采集系统状态（CPU、内存、磁盘、网络、时间、GPU等），
//       仅在状态字符串发生变化时发布 data 话题，避免管理节点累积无意义重复数据。
//       发布 info 话题维持心跳，提供固定描述文本。
//
// 参数:
//   agent_name   - 命名空间，默认 "agent"
//   publish_rate - 采集及 info 发布频率 (Hz)，默认 1.0
//
// 对外接口 (遵循 input 模块规范):
//   话题  /<agent_name>/input/system_status/info (cs_interfaces/msg/InputInfo)
//        desc: "系统状态", mode: "latest"
//   话题  /<agent_name>/input/system_status      (std_msgs/String)
//       内容: 自由格式状态文本，仅在内容变化时发布。
//             格式示例: "CPU:12% MEM:34% DISK:/:10GB/50GB NET:eth0:192.168.1.2 TIME:2025-... GPU:... HOST:myhost USER:leaf"

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
#include "cs_interfaces/msg/input_info.hpp"

using namespace std::chrono_literals;
using InputInfo = cs_interfaces::msg::InputInfo;

class SystemStatusNode : public rclcpp::Node {
public:
  SystemStatusNode(const std::string & agent_name)
  : Node("system_status_node", agent_name), agent_name_(agent_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("publish_rate", 1.0);
    double rate = this->get_parameter("publish_rate").as_double();

    // info 话题 (描述 + 心跳 + mode)
    rclcpp::QoS info_qos(1);
    info_qos.transient_local();
    info_qos.reliable();
    info_pub_ = this->create_publisher<InputInfo>(
      "/" + agent_name_ + "/input/system_status/info", info_qos);

    // data 话题
    data_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/input/system_status",
      rclcpp::QoS(1).reliable().transient_local());

    // 定时采集与心跳
    timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / rate),
      std::bind(&SystemStatusNode::tick, this));

    // 立即发布一次 info，避免启动初期被管理节点判定超时
    publish_info();
    RCLCPP_INFO(this->get_logger(), "SystemStatusNode ready");
  }

private:
  void publish_info() {
    InputInfo msg;
    msg.desc = "系统状态";
    msg.mode = "latest";
    info_pub_->publish(msg);
  }

  void tick() {
    // 维持心跳
    publish_info();

    // 采集并仅在变化时发布
    std::string text = build_status_string();
    if (!text.empty() && text != last_status_) {
      last_status_ = text;
      std_msgs::msg::String msg;
      msg.data = text;
      data_pub_->publish(msg);
    }
  }

  // ---------------------------------------------------------------
  // 构造自由格式状态文本（仅当任一采集项变化时整体重发）
  // ---------------------------------------------------------------
  std::string build_status_string() {
      std::ostringstream oss;

      // CPU
      int cpu = static_cast<int>(get_cpu_usage());
      oss << "CPU:" << cpu << "% ";

      // 内存
      struct sysinfo si;
      if (sysinfo(&si) == 0) {
          unsigned long total_mb = si.totalram * si.mem_unit / (1024 * 1024);
          unsigned long free_mb  = si.freeram  * si.mem_unit / (1024 * 1024);
          unsigned long used_mb  = total_mb - free_mb;
          oss << "MEM:" << used_mb << "MB/" << total_mb << "MB ";
      }

      // 磁盘 /
      struct statvfs sv;
      if (statvfs("/", &sv) == 0) {
          unsigned long long total_gb = sv.f_blocks * sv.f_frsize / (1024LL*1024*1024);
          unsigned long long avail_gb = sv.f_bavail * sv.f_frsize / (1024LL*1024*1024);
          unsigned long long used_gb  = total_gb - avail_gb;
          oss << "DISK:/:" << used_gb << "GB/" << total_gb << "GB ";
      }

      // 网络接口 (跳过 lo，取第一个非回环 IPv4)
      std::string net_info = "none";
      struct ifaddrs *ifaddr, *ifa;
      if (getifaddrs(&ifaddr) == 0) {
          std::ostringstream net_oss;
          bool first = true;
          for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
              if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                  std::string ifname(ifa->ifa_name);
                  if (ifname == "lo") continue;          // 跳过回环
                  char ip[INET_ADDRSTRLEN];
                  inet_ntop(AF_INET,
                            &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr,
                            ip, sizeof(ip));
                  if (!first) net_oss << ",";
                  first = false;
                  net_oss << ifname << ":" << ip;
              }
          }
          freeifaddrs(ifaddr);
          std::string collected = net_oss.str();
          if (!collected.empty()) net_info = collected;
      }
      oss << "NET:" << net_info << " ";

      // UTC 时间
      std::time_t now = std::time(nullptr);
      std::tm *gmt = std::gmtime(&now);
      char time_buf[64];
      std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmt);
      oss << "TIME:" << time_buf << " ";

      // GPU (如果有)
      std::string gpu = get_gpu_info();
      if (!gpu.empty()) {
          oss << "GPU:" << gpu << " ";
      }

      // 主机名
      char hostname[256];
      if (gethostname(hostname, sizeof(hostname)) == 0) {
          oss << "HOST:" << hostname << " ";
      }

      // 用户名
      const char* user = std::getenv("USER");
      oss << "USER:" << (user ? user : "unknown") << " ";

      // 机器码 (machine-id)
      std::ifstream mid_file("/etc/machine-id");
      std::string mid = "unknown";
      if (mid_file.is_open()) {
          std::getline(mid_file, mid);
          mid_file.close();
          // 去除可能的空白
          if (!mid.empty()) {
              mid.erase(mid.find_last_not_of(" \t\n\r") + 1);
          }
      }
      oss << "MID:" << (mid.empty() ? "unknown" : mid);

      return oss.str();
  }

  // 读取 /proc/stat 计算瞬时 CPU 使用率
  static double get_cpu_usage() {
    static unsigned long long prev_idle = 0, prev_total = 0;
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) return 0.0;
    std::string line;
    std::getline(stat, line);
    stat.close();

    char cpu[4];
    unsigned long long user, nice, system, idle, iowait, irq, softirq;
    sscanf(line.c_str(), "%s %llu %llu %llu %llu %llu %llu %llu",
           cpu, &user, &nice, &system, &idle, &iowait, &irq, &softirq);
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq;
    unsigned long long total_diff = total - prev_total;
    unsigned long long idle_diff  = idle - prev_idle;
    prev_total = total;
    prev_idle  = idle;
    if (total_diff == 0) return 0.0;
    return 100.0 * (total_diff - idle_diff) / total_diff;
  }

  // 通过 nvidia-smi 获取 GPU 简要信息 (如不可用则返回空)
  static std::string get_gpu_info() {
    FILE* fp = popen("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null", "r");
    if (!fp) return "";
    char buf[256];
    std::string result;
    if (fgets(buf, sizeof(buf), fp) != nullptr) {
      result = buf;
      // 去掉尾部换行
      if (!result.empty() && result.back() == '\n') result.pop_back();
    }
    pclose(fp);
    return result;
  }

  // 成员变量
  std::string agent_name_;
  rclcpp::Publisher<InputInfo>::SharedPtr info_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr data_pub_;
  rclcpp::TimerBase::SharedPtr timer_;

  std::string last_status_;   // 上一次发布的状态字符串
};

// ------------------------------------------------------------------
// main
// ------------------------------------------------------------------
int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);

  auto temp = std::make_shared<rclcpp::Node>("temp");
  temp->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp->get_parameter("agent_name").as_string();
  temp.reset();

  auto node = std::make_shared<SystemStatusNode>(agent_name);
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}