<div align="center">

[🇺🇸 English](README.md) | [🇨🇳 中文](README_zh.md) | [🇩🇪 Deutsch](README_de.md) | [🇪🇸 Español](README_es.md) | [🇫🇷 Français](README_fr.md) | [🇯🇵 日本語](README_ja.md) | [🇷🇺 Русский](README_ru.md)

</div>
<div align="center">


# Cloud-Soul

### *基于 ROS2 的 AI 智能体 — 一个灵魂，多端共生*

[![ROS2](https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros&style=for-the-badge)](https://docs.ros.org/en/humble/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B&style=for-the-badge)](https://en.cppreference.com/w/cpp/17)
[![MIT](https://img.shields.io/badge/License-MIT-97CA00?style=for-the-badge)](LICENSE)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu&style=for-the-badge)](https://releases.ubuntu.com/22.04/)

</div>

---

## 🧬 什么是 Cloud-Soul？

> **Cloud-Soul** 是一个基于 ROS2 构建的 AI 智能体框架。
> 把它想象成一个**数字生命体**——跨多台机器运行，
> 所有记忆同步到 Git 仓库。它思考、行动、记忆、进化。

```text
         ┌──────────────────────────────────────┐
         │            ☁️  Cloud Memory            │
         │              Git Repo                │
         │     diaries/ cognitions/ prompts/     │
         └──────┬──────────┬──────────┬─────────┘
                │          │          │
         ┌──────▼────┐ ┌───▼──────┐ ┌▼──────────┐
         │  🖥️  PC    │ │ 🍓  SBC  │ │ ☁️  Server │
         └───────────┘ └──────────┘ └───────────┘

      同一个灵魂。不同的躯体。同步的记忆。
```

---

## ⚡ 特性

|   | 特性 | 说明 |
|---|------|------|
| 🔄 | **多终端** | 一个 Agent，多台机器，无缝切换 |
| ☁️ | **Git 记忆** | 所有记忆自动推拉至 Git 仓库 |
| 🧩 | **插件工具** | ROS2 Action 节点即插即用，自动发现 |
| 🎛️ | **多渠道** | Web 聊天 / ROS2 / 邮件 / 终端 |
| 💭 | **思考模式** | 深度推理，流式思考输出 |
| ⚡ | **快插传感器** | 系统状态、消息输入、自定义传感器 |

---

## 🏗️ 架构

```
  ┌─────────────┐
  │  LLM (API)  │  DeepSeek / OpenAI 兼容
  └──────┬──────┘
         │
  ┌──────┴───────┐    ┌──────────────────────────────────┐
  │   cs_core    │◄───│          cs_output               │
  │              │    │                                  │
  │ agent_loop   │    │  shell_exec   file_rdwt          │
  │ memory_node  │    │  message_send web_search         │
  │ call_openai  │    │  output_mgmt (自动发现)           │
  └──────┬───────┘    └──────────────────────────────────┘
         │
  ┌──────┴───────┐
  │   cs_input   │
  │              │
  │ system_status│   CPU · 内存 · 磁盘 · 网络 · machine-id
  │ msg_receive  │   web_chat · ROS2 话题 · 邮件
  │ input_mgmt   │   快照聚合器
  └──────────────┘
```

> 三个包，无限工具。通过 ROS2 Actions 连接。

---

## 🚀 快速开始

```bash
# 1. 系统依赖
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# 2. 克隆 & 编译
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul && colcon build --symlink-install

# 3. 配置 (交互式向导)
./config

# 4. 启动
./start
```

> 打开 `http://localhost:8080` 进入 Web 聊天。

---

## 🧬 记忆模型

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md       ← 系统提示词 (实时引用 cognitions)
│   └── COMPRESS.md   ← 上下文压缩提示词
├── cognitions/
│   ├── SELF.md       ← 我是谁
│   ├── MASTER.md     ← 我的用户
│   ├── METHOD.md     ← 我的工作方式
│   └── WORLD.md      ← 我所知的世界
└── diaries/
    └── 20260629.md   ← 今日日志 (LLM 压缩)
```

---

## 🔧 扩展

**添加工具** — 一个文件，自动发现：

1. 创建 `src/cs_output/src/my_tool_node.cpp`
2. 发布 info 至 `/{agent}/output/my_tool/info` (transient_local)
3. 在 `/{agent}/output/my_tool` 提供 `ExecuteTool` action

**添加传感器** — 同理：

1. 发布 `InputInfo` 至 `/{agent}/input/my_sensor/info`
2. 发布数据至 `/{agent}/input/my_sensor`

> `input_mgmt_node` 和 `output_mgmt_node` 自动处理其余工作。

---

## 📊 状态

| 指标 | 值 |
|------|-----|
| 最新版本 | `v0.3.3-Beta` |
| 包数量 | 4 (`cs_core` `cs_input` `cs_output` `cs_interfaces`) |
| 工具 | 5 (`shell_exec` `file_rdwt` `message_send` `web_search` `web_chat`) |
| 传感器 | 3 (`system_status` `message_receive` `ros_msg`) |
| 节点 | 10+ |

---

<div align="center">

**用 ❤️ 构建于 ROS2 Humble**

*"灵魂在云端，躯体无处不在。"*

</div>