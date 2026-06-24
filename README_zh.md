# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](#) | [日本語](README_ja.md) | [Русский](README_ru.md) | [Français](README_fr.md) | [Deutsch](README_de.md) | [Español](README_es.md)

基于 ROS2 的 AI Agent 运行时。单一 Agent，多终端，云端同步记忆。

---

## 🏗️ 架构

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

三个包，通过 ROS2 action `/agent_loop/_action/execute_tool` 通信：

| 包 | 职责 |
|----|------|
| `cs_input` | 感知层：系统状态采集、用户消息订阅 |
| `cs_core` | 核心推理：LLM ⇄ 工具调用循环、Git 记忆管理 |
| `cs_output` | 工具层：Shell 执行、文件读写、消息发送、网页搜索、Web 聊天 |

---

## ⚙️ 工作原理

```
  system_status  ──┐
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

## 🚀 快速开始

**前置依赖**: Ubuntu 22.04, ROS2 Humble, DeepSeek 兼容的 API key。

```bash
# 系统依赖
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# 克隆 & 构建
git clone git@github.com:your-org/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**记忆仓库** — fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory)，配置 SSH 推送。

**首次配置** (交互式向导)：

```bash
./config
```

生成 `~/.cloudsoul/config.yaml`，配置 Agent 名称、API 凭据、仓库 URL 和各项调优参数。

**一键启动**：

```bash
./start
```

自动启动 `cs_input`、`cs_output`、`cs_core` 和 `web_chat`（Flask SSE，端口 8080）。日志输出至 `/tmp/cloudsoul_*.log`。

**以 root 运行** (需要系统级权限时)：

```bash
sudo -E ./start
```

所有节点均支持 `respawn=True` — 节点崩溃时 ROS2 自动重启。

---

## 🧠 记忆模型

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md        # 系统提示词 (引用 [cognitions/*.md])
│   └── COMPRESS.md    # 压缩提示词
├── cognitions/
│   ├── SELF.md        # Agent 身份设定
│   ├── MASTER.md      # 用户画像
│   ├── METHOD.md      # 行为规则
│   └── WORLD.md       # 已知事实
└── diaries/
    └── YYYYMMDD.md    # 每日日志 (LLM 压缩)
```

---

## 🔌 节点

### cs_core

| 节点 | 描述 |
|------|------|
| `agent_loop_node` | 主循环：快照 → LLM → 工具 → 循环。上下文管理 |
| `memory_node` | Git 记忆召回/归档：拉取认知，推送日记 |

### cs_input

| 节点 | 话题 | 描述 |
|------|------|------|
| `system_status_node` | `/{agent}/input/system_status` | CPU、内存、磁盘、网络、主机名、machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | ROS2 / web_chat 消息接收 |
| `input_mgmt_node` | `/{agent}/input` (srv) | 聚合传感器数据为快照 |

### cs_output

| 节点 | Action | 描述 |
|------|--------|------|
| `shell_exec_node` | execute_tool | Shell 命令执行 |
| `file_read_node` | execute_tool | 文件读取 (偏移/长度/编码) |
| `file_write_node` | execute_tool | 文件写入 (覆盖/追加) |
| `message_send_node` | execute_tool | 邮件 (s-nail)、ROS2 发布或 web_chat 回复 |
| `web_search_node` | execute_tool | 网页搜索，多引擎自动切换 |
| `web_chat_server.py` | (Flask SSE) | 浏览器聊天界面 (端口 8080) |
| `output_mgmt_node` | execute_tool | 自动发现并路由调用至工具节点 |

---

## 🔧 配置

所有参数集中在 `~/.cloudsoul/config.yaml`（由 `./config` 生成）。

| 参数 | 节点 | 默认值 |
|------|------|--------|
| `agent_name` | 全部 | `agent` |
| `repo.url` | memory_node | — 必填 |
| `loop.max_context_tokens` | agent_loop_node | `200000` |
| `loop.summary_turns` | agent_loop_node | `25` |
| `loop.tool_timeout` | cs_core / cs_output | `60.0` |
| `input.info_timeout` | input mgmt | `3.0` |
| `output.info_timeout` | output mgmt | `3.0` |

LLM 配置在 `config.yaml` 的 `llms` 列表中，`default_llm` 字段指定当前使用的 LLM。

---

## 🎨 自定义

1. Fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory)，编辑 `cognitions/`
2. 添加工具：在 `cs_output/src/` 新建节点，发布 `/{agent}/output/<name>/info` 话题（JSON 工具描述，`std_msgs/String`，QoS transient_local），`output_mgmt_node` 自动发现
3. 添加传感器：在 `cs_input/src/` 新建节点，发布 `/{agent}/input/<name>/info` 话题（`InputInfo` 消息，QoS transient_local）和 `/{agent}/input/<name>` 数据，`input_mgmt_node` 自动发现
4. `cs_interfaces/include/cs_interfaces/constants.hpp` — 统一管理所有超时、错误码和消息文本

---

## 📄 许可证

MIT