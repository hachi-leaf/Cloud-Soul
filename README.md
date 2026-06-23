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

## Architecture

```
                  ┌─────────────┐
                  │  LLM (API)  │
                  └──────┬──────┘
                         │
┌────────────┐    ┌──────┴──────┐    ┌─────────────────────────────┐
│  cs_input  │───→│   cs_core    │───→│         cs_output            │
│            │    │              │    │                              │
│ system_    │    │ agent_loop   │    │ shell_exec    file_read     │
│ status     │    │ memory_node  │    │ file_write    message_send  │
│ message_   │    │ call_openai  │    │ web_search                  │
│ receive    │    │              │    │                              │
│ input_mgmt │    │ Git repo ↔   │    │ output_mgmt                  │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

Three packages, connected via ROS2 action `/agent_loop/_action/execute_tool`:

| Package | Role |
|---------|------|
| `cs_input` | Sensors: system status, user message subscription |
| `cs_core` | Agent loop (LLM ⇄ tools), Git-based memory |
| `cs_output` | Tools: shell, file I/O, messaging, web search |

---

## How It Works

```
  system_status ──┐
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

## Quick Start

**Prerequisites**: Ubuntu 22.04, ROS2 Humble, DeepSeek API key (or compatible endpoint).

```bash
# deps
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev

# build
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Memory repo** — fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), configure SSH push.

**Launch**:

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

export OPENAI_API_KEY=sk-xxx
export OPENAI_BASE_URL=https://api.deepseek.com/v1
export OPENAI_MODEL=deepseek-chat

ros2 run cs_core memory_node --ros-args \
  -p repo_url:="git@github.com:your/agent-soul.git" \
  -p repo_dir:="$HOME/.adam/soul_repo" &

ros2 run cs_input system_status_node &
ros2 run cs_input message_receive_node --ros-args \
  -p topic_name:="/adam/input/message_receive" &

ros2 run cs_input input_mgmt_node &
ros2 run cs_output output_mgmt_node &
ros2 run cs_output shell_exec_node &
ros2 run cs_output file_read_node &
ros2 run cs_output file_write_node &
ros2 run cs_output message_send_node &
ros2 run cs_output web_search_node &

ros2 run cs_core agent_loop_node
```

---

## Memory Model

```
~/.adam/soul_repo/
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

## Nodes

### cs_core

| Node | Description |
|------|-------------|
| `agent_loop_node` | Main loop: snapshot → LLM → tools → repeat. Context management |
| `memory_node` | Git recall/archive: pull cognitions, push diaries |

### cs_input

| Node | Topic | Description |
|------|-------|-------------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, MEM, disk, net, hostname, machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | ROS2 String subscriber |
| `input_mgmt_node` | `/{agent}/input/snapshot` (srv) | Aggregates sensor data |

### cs_output

| Node | Action | Description |
|------|--------|-------------|
| `shell_exec_node` | execute_tool | Shell command execution |
| `file_read_node` | execute_tool | File read (offset/length/encoding) |
| `file_write_node` | execute_tool | File write (overwrite/append) |
| `message_send_node` | execute_tool | Email (s-nail) or ROS2 publish |
| `web_search_node` | execute_tool | HTTP GET, multi-engine fallback |
| `output_mgmt_node` | execute_tool | Routes calls to tools |

---

## Configuration

| Parameter | Node | Default |
|-----------|------|---------|
| `agent_name` | all | `adam` |
| `repo_url` | memory_node | — required |
| `repo_dir` | memory_node | `~/.adam/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `topic_name` | message_receive_node | `/adam/input/message_receive` |

Environment: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## Customization

1. Fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), edit `cognitions/`
2. Add tools: new node in `cs_output/src/`, register in `output_mgmt_node.cpp`
3. Add sensors: new node in `cs_input/src/`, register in `input_mgmt_node.cpp`

---

## License

MIT