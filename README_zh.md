# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](#) | [日本語](README_ja.md) | [Русский](README_ru.md) | [Français](README_fr.md) | [Deutsch](README_de.md) | [Español](README_es.md)

基于 ROS2 的 AI Agent 运行时框架。一个智能体、多终端部署、云端记忆同步。

---

## 架构

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
│ input_mgmt │    │ Git 仓库 ↔   │    │ output_mgmt                  │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

三个包，通过 ROS2 action `/agent_loop/_action/execute_tool` 通信：

| 包 | 职责 |
|----|------|
| `cs_input` | 感知层：系统状态采集、用户消息订阅 |
| `cs_core` | 核心推理：LLM ⇄ 工具调用循环、Git 记忆管理 |
| `cs_output` | 工具层：Shell 执行、文件读写、消息发送、网页搜索 |

---

## 工作原理

```
  system_status ──┐
  message_receive ─┤
                   ├──→ input_mgmt (快照) ──→ agent_loop
                   │                              │
                   │     ┌────────────────────────┘
                   │     ▼
                   │   LLM: 推理 → 工具调用
                   │     │
                   │     ▼
                   │   output_mgmt → tool_node → 结果
                   │     │
                   │     ▼
                   │   memory_node: 归档至 Git 日记
                   │     │
                   └─────┘ (下一轮)
```

- 收到用户消息 → 立即处理
- 空闲时 → 以系统状态心跳驱动循环
- 上下文超过阈值 → LLM 压缩至日记后重置
- 记忆以 Markdown 存于 Git 仓库，每轮 pull/push

---

## 快速开始

**前置条件**: Ubuntu 22.04, ROS2 Humble, DeepSeek API key（或兼容端点）。

```bash
# 依赖
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev

# 编译
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**记忆仓库** — fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul)，配置 SSH 推送。

**启动**:

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

## 记忆模型

```
~/.adam/soul_repo/
├── prompts/
│   ├── RULE.md        # 系统提示词（引用 [cognitions/*.md]）
│   └── COMPRESS.md    # 压缩提示词
├── cognitions/
│   ├── SELF.md        # Agent 自我认知
│   ├── MASTER.md      # 使用者档案
│   ├── METHOD.md      # 行为准则
│   └── WORLD.md       # 世界认知
└── diaries/
    └── YYYYMMDD.md    # 日记（LLM 自动摘要）
```

---

## 节点

### cs_core

| 节点 | 说明 |
|------|------|
| `agent_loop_node` | 主循环：快照 → LLM → 工具调用 → 重复。管理上下文窗口 |
| `memory_node` | Git 记忆管理：拉取认知文件，推送日记 |

### cs_input

| 节点 | 话题 | 说明 |
|------|------|------|
| `system_status_node` | `/{agent}/input/system_status` | CPU、内存、磁盘、网络、主机名、机器码（1 Hz） |
| `message_receive_node` | `/{agent}/input/message_receive` | ROS2 String 订阅 |
| `input_mgmt_node` | `/{agent}/input/snapshot` (srv) | 聚合传感器数据 |

### cs_output

| 节点 | Action | 说明 |
|------|--------|------|
| `shell_exec_node` | execute_tool | Shell 命令执行 |
| `file_read_node` | execute_tool | 文件读取（偏移/长度/编码） |
| `file_write_node` | execute_tool | 文件写入（覆盖/追加） |
| `message_send_node` | execute_tool | 邮件（s-nail）或 ROS2 发布 |
| `web_search_node` | execute_tool | HTTP 请求，多源切换 |
| `output_mgmt_node` | execute_tool | 工具路由分发 |

---

## 配置

| 参数 | 节点 | 默认值 |
|------|------|--------|
| `agent_name` | 全部 | `adam` |
| `repo_url` | memory_node | — 必填 |
| `repo_dir` | memory_node | `~/.adam/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `topic_name` | message_receive_node | `/adam/input/message_receive` |

环境变量: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## 自定义

1. Fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul)，编辑 `cognitions/`
2. 添加工具：在 `cs_output/src/` 中新建节点，在 `output_mgmt_node.cpp` 中注册
3. 添加传感器：在 `cs_input/src/` 中新建节点，在 `input_mgmt_node.cpp` 中注册

---

## 许可证

MIT