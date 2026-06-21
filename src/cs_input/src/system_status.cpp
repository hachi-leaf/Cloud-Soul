// Copyright (c) leaf
// SPDX-License-Identifier: MIT

// 节点: system_status (输入源)
// 发布 /<agent_name>/input/system_status      (data)
// 发布 /<agent_name>/input/system_status/desc (desc)
//
// 采集内容: CPU使用率、内存、磁盘、网络接口、UTC时间、GPU(如有)、主机名、用户名等。
// 频率可调，默认 1 Hz。
//
// 参数:
//   agent_name   - 命名空间前缀，默认 "agent"
//   publish_rate - data 话题发布频率 (Hz)，默认 1.0

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

using namespace std::chrono_literals;

class SystemStatusNode : public rclcpp::Node {
public:
  SystemStatusNode(const std::string & agent_name, const std::string & source_name)
  : Node(source_name), agent_name_(agent_name), source_name_(source_name)
  {
    this->declare_parameter<std::string>("agent_name", agent_name);
    this->declare_parameter<double>("publish_rate", 1.0);
    double rate = this->get_parameter("publish_rate").as_double();

    // data 话题
    data_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/input/" + source_name_, rclcpp::QoS(1).reliable());

    // desc 话题 (transient local + reliable)
    rclcpp::QoS desc_qos(1);
    desc_qos.transient_local();
    desc_qos.reliable();
    desc_pub_ = this->create_publisher<std_msgs::msg::String>(
      "/" + agent_name_ + "/input/" + source_name_ + "/desc", desc_qos);

    // 定时发布数据
    data_timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / rate),
      std::bind(&SystemStatusNode::publish_data, this));

    // 定期重发 desc (心跳)
    desc_timer_ = this->create_wall_timer(
      1s, std::bind(&SystemStatusNode::publish_desc, this));

    publish_desc();
    RCLCPP_INFO(this->get_logger(), "system_status 输入源已启动, agent: %s", agent_name_.c_str());
  }

private:
  void publish_desc() {
    std_msgs::msg::String msg;
    // 使用自定义定界符避免括号冲突
    msg.data = R"json(
      {
        "name": "system_status",
        "type": "sensor",
        "description": "系统全量状态：CPU、内存、磁盘、网络接口、UTC时间、GPU(可选)、主机名、用户名。",
        "data_schema": {
          "cpu_percent": "number",
          "mem_total_mb": "number",
          "mem_used_mb": "number",
          "disk": "array of {mount, total_gb, used_gb, avail_gb}",
          "net_interfaces": "array of {name, address}",
          "utc_time": "ISO8601 string",
          "gpu": "string (info or empty)",
          "hostname": "string",
          "username": "string",
          "machine_id": "string"
        }
      }
    )json";
    desc_pub_->publish(msg);
  }

  void publish_data() {
    std_msgs::msg::String msg;
    msg.data = collect_status_json();
    data_pub_->publish(msg);
  }

  std::string collect_status_json() {
    std::ostringstream json;
    json << "{";

    // CPU 使用率
    json << "\"cpu_percent\":" << get_cpu_usage() << ",";

    // 内存
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
      unsigned long total_mb = si.totalram * si.mem_unit / (1024 * 1024);
      unsigned long free_mb = si.freeram * si.mem_unit / (1024 * 1024);
      unsigned long used_mb = total_mb - free_mb;
      json << "\"mem_total_mb\":" << total_mb << ","
           << "\"mem_used_mb\":" << used_mb << ",";
    } else {
      json << "\"mem_total_mb\":0,\"mem_used_mb\":0,";
    }

    // 磁盘
    json << "\"disk\":[";
    append_disk_info(json, "/");
    append_disk_info(json, "/home");
    // 去掉末尾逗号
    std::string disk_str = json.str();
    if (disk_str.back() == ',') {
      disk_str.pop_back();
      json.str("");
      json << disk_str;
    }
    json << "],";

    // 网络接口
    json << "\"net_interfaces\":[";
    bool first_if = true;
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
      for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
          char addr[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, addr, sizeof(addr));
          if (!first_if) json << ",";
          first_if = false;
          json << "{\"name\":\"" << ifa->ifa_name << "\",\"address\":\"" << addr << "\"}";
        }
      }
      freeifaddrs(ifaddr);
    }
    json << "],";

    // UTC 时间
    std::time_t now = std::time(nullptr);
    std::tm *gmt = std::gmtime(&now);
    char time_buf[64];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmt);
    json << "\"utc_time\":\"" << time_buf << "\",";

    // GPU
    json << "\"gpu\":\"" << get_gpu_info() << "\",";

    // 主机名
    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    // machine-id
    std::string mid;
    std::ifstream mid_file("/etc/machine-id");
    if (mid_file) { std::getline(mid_file, mid); mid_file.close(); }
    if (mid.empty()) mid = "unknown";
    json << "\"hostname\":\"" << hostname << "\",";

    // 用户名
    const char* user = std::getenv("USER");
    if (!user) user = "unknown";
    json << "\"username\":\"" << user << "\","
         << "\"machine_id\":\"" << mid << "\"";

    json << "}";
    return json.str();
  }

  // 辅助函数：CPU 使用率 (简化计算)
  double get_cpu_usage() {
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
    unsigned long long idle_diff = idle - prev_idle;
    prev_total = total;
    prev_idle = idle;
    if (total_diff == 0) return 0.0;
    return 100.0 * (total_diff - idle_diff) / total_diff;
  }

  // 添加磁盘信息
  void append_disk_info(std::ostringstream & json, const std::string & path) {
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) return;
    unsigned long long total = stat.f_blocks * stat.f_frsize;
    unsigned long long avail = stat.f_bavail * stat.f_frsize;
    unsigned long long used = total - avail;
    json << "{\"mount\":\"" << path << "\","
         << "\"total_gb\":" << (total / (1024.0 * 1024 * 1024)) << ","
         << "\"used_gb\":" << (used / (1024.0 * 1024 * 1024)) << ","
         << "\"avail_gb\":" << (avail / (1024.0 * 1024 * 1024)) << "},";
  }

  // GPU 信息
  std::string get_gpu_info() {
    FILE* fp = popen("nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null", "r");
    if (!fp) return "";
    char buf[256];
    std::string result;
    if (fgets(buf, sizeof(buf), fp) != nullptr) {
      result = std::string(buf);
      if (!result.empty() && result.back() == '\n') result.pop_back();
    }
    pclose(fp);
    return result;
  }

  std::string agent_name_;
  std::string source_name_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr data_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr desc_pub_;
  rclcpp::TimerBase::SharedPtr data_timer_;
  rclcpp::TimerBase::SharedPtr desc_timer_;
};

int main(int argc, char ** argv) {
  rclcpp::init(argc, argv);
  auto temp = std::make_shared<rclcpp::Node>("temp");
  temp->declare_parameter<std::string>("agent_name", "agent");
  std::string agent_name = temp->get_parameter("agent_name").as_string();
  temp.reset();

  auto node = std::make_shared<SystemStatusNode>(agent_name, "system_status");
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}