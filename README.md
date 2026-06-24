# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](#) | [中文](README_zh.md) | [日本語](README_ja.md) | [Русский](README_ru.md) | [Français](README_fr.md) | [Deutsch](README_de.md) | [Español](README_es.md)

ROS2-based AI agent runtime. One agent, multiple terminals, cloud-synced memory.

---

## 🏗️ Architecture

```
                  ┌─────────────┐
                  │  LLM (API)  │
                  └──────┬──────┘
                         │
┌────────────┐    ┌──────┴───────┐    ┌─────────────────────────────┐
│  cs_input  │───→│   cs_core    │───→│         cs_output           │
│            │    │              │    │                             │
│ system_    │    │ agent_loop   │    │ shell_exec    file_read     │
│ status     │    │ memory_node  │    │ file_write    message_send  │
│ message_   │    │ call_openai  │    │ web_search    web_chat      │
│ receive    │    │              │    │                             │
│ input_mgmt │    │ Git repo ↔   │    │ output_mgmt                 │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

Three packages, connected via ROS2 action `/agent_loop/_action/execute_tool`:

| Package | Role |
|---------|------|
| `cs_input` | Sensors: system status, user message subscription |
| `cs_core` | Agent loop (LLM ⇄ tools), Git-based memory |
| `cs_output` | Tools: shell, file I/O, messaging, web search, web chat |

---

## ⚙️ How It Works

```
  system_status  ──┐
  message_receive ─┤
                   ├──→ input_mgmt (snapshot) ──→ agent_loop
                   │                                  │
                   │     ┌────────────────────────────┘
                   │     ▼
                   │   LLM: reasoning → tool_calls
                   │     │
                   │     ▼
                   │   output_mgmt → tool_node → result
                   │     │
                   │     ▼
                   │   memory_node: archive to Git diary
                   │     │
                   └─────┘ (next cycle)
```

- User message arrives → immediate processing
- Idle → loops on system_status heartbeat
- Context exceeds threshold → LLM compresses to diary, resets
- Memory persists as Markdown in Git repo; `pull`/`push` per cycle

---

## 🚀 Quick Start

**Prerequisites**: Ubuntu 22.04, ROS2 Humble, a DeepSeek-compatible API key.

```bash
# system deps
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# clone & build
git clone git@github.com:your-org/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Memory repo** — fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), configure SSH push.

**One-time setup** (interactive wizard):

```bash
./config
```

Creates `~/.cloudsoul/config.yaml` with agent name, API credentials, repo URL, and all tuning parameters.

**Launch everything** (one command):

```bash
./start
```

This starts `cs_input`, `cs_output`, `cs_core`, and `web_chat` (Flask SSE on port 8080). Logs go to `/tmp/cloudsoul_*.log`.

**Run as root** (for system-level access):

```bash
sudo -E ./start
```

All nodes support `respawn=True` — ROS2 auto-restarts crashed nodes.

---

## 🧠 Memory Model

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md        # system prompt (refs [cognitions/*.md])
│   └── COMPRESS.md    # compression prompt
├── cognitions/
│   ├── SELF.md        # agent identity
│   ├── MASTER.md      # user profile
│   ├── METHOD.md      # behavioral rules
│   └── WORLD.md       # known facts
└── diaries/
    └── YYYYMMDD.md    # daily log (LLM-compressed)
```

---

## 🔌 Nodes

### cs_core

| Node | Description |
|------|-------------|
| `agent_loop_node` | Main loop: snapshot → LLM → tools → repeat. Context management |
| `memory_node` | Git recall/archive: pull cognitions, push diaries |

### cs_input

| Node | Topic | Description |
|------|-------|-------------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, MEM, disk, net, hostname, machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | ROS2 / web_chat message receiver |
| `input_mgmt_node` | `/{agent}/input` (srv) | Aggregates sensor data into snapshot |

### cs_output

| Node | Action | Description |
|------|--------|-------------|
| `shell_exec_node` | execute_tool | Shell command execution |
| `file_read_node` | execute_tool | File read (offset/length/encoding) |
| `file_write_node` | execute_tool | File write (overwrite/append) |
| `message_send_node` | execute_tool | Email (s-nail), ROS2 publish, or web_chat reply |
| `web_search_node` | execute_tool | Web search with multi-engine fallback |
| `web_chat_server.py` | (Flask SSE) | Browser-based chat interface (port 8080) |
| `output_mgmt_node` | execute_tool | Auto-discovers and routes calls to tool nodes |

---

## 🔧 Configuration

All parameters live in `~/.cloudsoul/config.yaml` (generated by `./config`).

| Parameter | Node | Default |
|-----------|------|---------|
| `agent_name` | all | `agent` |
| `repo.url` | memory_node | — required |
| `loop.max_context_tokens` | agent_loop_node | `200000` |
| `loop.summary_turns` | agent_loop_node | `25` |
| `loop.tool_timeout` | cs_core / cs_output | `60.0` |
| `input.info_timeout` | input mgmt | `3.0` |
| `output.info_timeout` | output mgmt | `3.0` |

LLM settings are configured inside the `llms` list in `config.yaml`; the `default_llm` field picks the active one.

---

## 🎨 Customization

1. Fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), edit `cognitions/`
2. Add tools: create a node in `cs_output/src/` that publishes `/{agent}/output/<name>/info` (JSON tool descriptor, `std_msgs/String`, QoS transient_local). `output_mgmt_node` auto-discovers it.
3. Add sensors: create a node in `cs_input/src/` that publishes `/{agent}/input/<name>/info` (`InputInfo` msg, QoS transient_local) and data on `/{agent}/input/<name>`. `input_mgmt_node` auto-discovers it.
4. `cs_interfaces/include/cs_interfaces/constants.hpp` — all timeouts, error codes, and message strings in one place

---

## 📄 License

MIT