<div align="center">

[🇺🇸 English](README.md) | [🇨🇳 中文](README_zh.md) | [🇩🇪 Deutsch](README_de.md) | [🇪🇸 Español](README_es.md) | [🇫🇷 Français](README_fr.md) | [🇯🇵 日本語](README_ja.md) | [🇷🇺 Русский](README_ru.md)

</div>

<div align="center">

# Cloud-Soul

### *ROS2-Native AI Agent — One Soul, Everywhere*

[![ROS2](https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros&style=for-the-badge)](https://docs.ros.org/en/humble/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B&style=for-the-badge)](https://en.cppreference.com/w/cpp/17)
[![MIT](https://img.shields.io/badge/License-MIT-97CA00?style=for-the-badge)](LICENSE)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu&style=for-the-badge)](https://releases.ubuntu.com/22.04/)

</div>

---

## 🧬 What is Cloud-Soul?

> **Cloud-Soul** is an AI agent framework built on ROS2.
> Think of it as a **digital being** that lives across multiple machines,
> with a single memory synced to a Git repository.
> It thinks, acts, remembers, and evolves.

```text
         ┌──────────────────────────────────────┐
         │         ☁️☁️  Cloud Memory          │
         │              Git Repo                │
         │     diaries/ cognitions/ prompts/    │
         └─────┬────────────┬─────────────┬─────┘
               │            │             │
         ┌─────▼─────┐ ┌────▼─────┐ ┌─────▼─────┐
         │  🖥️PC🖥️  │ │ 🍓SBC🍓 │ │☁️Server☁️│
         └───────────┘ └──────────┘ └───────────┘

      Same Soul. Different Body. Synced Memory.
```

---

## ⚡ Features

|   | Feature | Description |
|---|---------|-------------|
| 🔄 | **Multi-Terminal** | One agent, many machines — switch seamlessly |
| ☁️ | **Git Memory** | All memories auto push/pull to Git repo |
| 🧩 | **Plugin Tools** | Auto-discovered ROS2 action nodes |
| 🎛️ | **Multi-Channel** | Web Chat / ROS2 / Email / Terminal |
| 💭 | **Thinking Mode** | Deep reasoning with streaming thought output |
| ⚡ | **Snap-In Sensors** | System status, messages, custom inputs |

---

## 🏗️ Architecture

```
  ┌─────────────┐
  │  LLM (API)  │  DeepSeek / OpenAI compatible
  └──────┬──────┘
         │
  ┌──────┴───────┐    ┌──────────────────────────────────┐
  │   cs_core    │◄───│          cs_output               │
  │              │    │                                  │
  │ agent_loop   │    │  shell_exec   file_rdwt          │
  │ memory_node  │    │  message_send                   │
  │ call_openai  │    │  output_mgmt (auto-discover)     │
  └──────┬───────┘    └──────────────────────────────────┘
         │
  ┌──────┴───────┐
  │   cs_input   │
  │              │
  │ system_status│   CPU · MEM · Disk · Net · machine-id
  │ msg_receive  │   web_chat · ROS2 topic · email
  │ input_mgmt   │   snapshot aggregator
  └──────────────┘
```

> Three packages, infinite tools. Connected via ROS2 Actions.

---

## 🚀 Quick Start

```bash
# 1. System Deps
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# 2. Clone & Build
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul && colcon build --symlink-install

# 3. Configure (interactive wizard)
./config

# 4. Launch
./start
```

> Open `http://localhost:8080` for web chat.

---

## 🧬 Memory Model

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md       ← System prompt (live-refs cognitions)
│   └── COMPRESS.md   ← Context compression prompt
├── cognitions/
│   ├── SELF.md       ← Who am I
│   ├── MASTER.md     ← My user
│   ├── METHOD.md     ← How I work
│   └── WORLD.md      ← What I know
└── diaries/
    └── 20260629.md   ← Today's log (LLM-compressed)
```

---

## 🔧 Extend

**Add a Tool** — one file, auto-discovered:

1. Create `src/cs_output/src/my_tool_node.cpp`
2. Publish info to `/{agent}/output/my_tool/info` (transient_local)
3. Serve `ExecuteTool` action at `/{agent}/output/my_tool`

**Add a Sensor** — same pattern:

1. Publish `InputInfo` to `/{agent}/input/my_sensor/info`
2. Publish data to `/{agent}/input/my_sensor`

> `input_mgmt_node` and `output_mgmt_node` handle the rest.


---

## 📝 Release Notes


### v0.4.0-Beta (2026-07-01)

**Portal 节点全链路改造 + 基础设施升级**

- **web_search_node**: 新增门户工具，支持 bing 搜索，HTML 抓取+正则提取，对齐 shell_exec_node 代码风格。
- **web_fetch_node**: 新增门户工具，全链路 zero-regex 处理（strip_blocks/strip_tags/decode_html_entities/extract_text 全部按字符扫描 O(n)，解决大 HTML 上 std::regex 栈溢出 SIGSEGV 崩溃），线程化改造（work_thread 异步执行不阻塞 executor），支持 config.yaml 参数化（max_results/max_size_mb）。
- **skills_loader_node**: 完整重写。线程化、构造函数+temp node、info desc 改为全量 frontmatter 文本（让 LLM 看到完整 skill 加载条件）、扫描时机改为启动+list+load 三时机。
- **4 个 portal 节点代码风格统一**: 全部对齐 shell_exec_node 结构——构造函数接受 agent_name、main 中 temp node 获取、handle_cancel/handle_accepted 命名、canceled_ 原子变量、错误路径 abort+exit_code=1。
- **Fast-DDS → CycloneDDS**: 根治多节点 libfastrtps 段错误（participant 泄漏/discovery 竞态），安装 ros-humble-rmw-cyclonedds-cpp，start 脚本设置 RMW_IMPLEMENTATION=rmw_cyclonedds_cpp。
- **web_chat 文件上传**: /send 附件消息包含完整文件路径 `[附件: name → /tmp/web_uploads/xxx]`，Agent 可直接读取。
- **start 脚本修复**: 恢复 repo_dir 传参（被 max_results/max_size_mb 插入覆盖导致 skills_loader 启动失败）。
- **METHOD.md 更新**: 记录 Fast-DDS 稳定性问题 + web_fetch regex 栈溢出教训。
### v0.3.6-Beta (2026-06-30)

**output_mgmt + output nodes 完整重构**

- **output_mgmt_node**: 重写为通用反射层。自动发现 `/agent_name/output/*/info` 话题，解析工具 schema JSON，注册 Action 客户端，运行时转发 tool_calls 到对应节点并回传结果。支持优雅退出 `cancel_all_goals()`。
- **shell_exec_node**: 迁移到 actionlib 并使用新统一协议。支持 `kill_and_wait`(先 SIGTERM 后 SIGKILL)、超时控制、`max_output_lines` 限制。
- **file_rdwt_node**: 迁移到 actionlib 并使用新统一协议。支持 read/write/append/insert/read_write 操作、行范围读取、UTF-8 sanitize。
- **message_send_node**: 迁移到 actionlib 并使用新统一协议。支持 email(s-nail)/ros_msg/web_chat 三渠道，TLS 超时自适应。
- **协议统一**: 所有 output 节点使用 `{"name":"tool","arguments":{...}}` 格式。validate_args 函数确保必填字段校验。
- **agent_loop_node**: 修复记忆压缩后首个保留消息为 tool 角色导致的 LLM 错误，添加 leading tool 消息剥离逻辑。
- **cs_interfaces**: 新增 `ToolCall.action` (`name`, `arguments`, `result`, `error`)，替代旧的独立 action 定义。

---

## 📊 Status

| Metric | Value |
|--------|-------|
| Latest | `v0.4.0-Beta` |
| Packages | 4 (`cs_core` `cs_input` `cs_output` `cs_interfaces`) |
| Tools | 6 (`shell_exec` `file_rdwt` `message_send` `web_search` `web_fetch` `skills_loader`) |
| Sensors | 3 (`system_status` `message_receive` `ros_msg`) |
| Nodes | 13+ |

---

<div align="center">

**Built with ❤️ on ROS2 Humble**

*"The soul is in the cloud, the body is everywhere."*

</div>
