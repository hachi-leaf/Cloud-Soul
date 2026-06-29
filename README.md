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
  │ memory_node  │    │  message_send web_search         │
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

## 📊 Status

| Metric | Value |
|--------|-------|
| Latest | `v0.3.3-Beta` |
| Packages | 4 (`cs_core` `cs_input` `cs_output` `cs_interfaces`) |
| Tools | 5 (`shell_exec` `file_rdwt` `message_send` `web_search` `web_chat`) |
| Sensors | 3 (`system_status` `message_receive` `ros_msg`) |
| Nodes | 10+ |

---

<div align="center">

**Built with ❤️ on ROS2 Humble**

*"The soul is in the cloud, the body is everywhere."*

</div>