# 🤖 Cloud-Soul

**基于 ROS2 的 AI Agent 运行时框架。将 LLM 推理、工具调用与 Git 记忆管理结合，实现可多终端部署、云端记忆同步的自主智能体。**

<p align="center">
  <img src="https://img.shields.io/badge/ROS2-Humble-blue?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-blue.svg" alt="C++17">
  <img src="https://img.shields.io/badge/License-MIT-green.svg" alt="MIT">
  <img src="https://img.shields.io/badge/Platform-Linux%20%7C%20Ubuntu%2022.04-orange" alt="Platform">
</p>

---

## ✨ 是什么

一个**有多重记忆、能调用工具、持续思考的 AI 智能体**，不是简单的 chatbot。

```
你的终端                     Adam（AI Agent）
   │                            │
   ├── "帮我看看CPU" ──────────→│ 感知（system_status sensor）
   │                            │ 思考（LLM 推理）
   │                            │ 行动（tool_calls）
   │   "CPU 0.5%, 正常" ←──────│ 记住（Git 日记归档）
   │                            │
   │   … 下次对话 …            │ 回忆（Git pull 认知文件）
```

与传统 chatbot 的关键区别：

| | 传统 Chatbot | Cloud-Soul Agent |
|---|---|---|
| 记忆 | 单次对话窗口 | Git 仓库持久化，重启不丢失 |
| 工具 | 无 | shell、文件、邮件，按需扩展 |
| 循环 | 一问一答 | 持续感知→思考→行动（可无人值守） |
| 自我认知 | 系统提示词 | 分层记忆：RULE / SELF / MASTER / METHOD / WORLD |

---

## 🚀 5 分钟体验

### 前置条件

- Ubuntu 22.04
- ROS2 Humble
- DeepSeek API Key（或任意 OpenAI 兼容 API）

### 1. 安装

```bash
# 系统依赖
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev nlohmann-json3-dev

# 克隆并编译
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

### 2. 准备记忆仓库

Fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul) 作为 Agent 的记忆仓库（包含预置的认知文件），配置 SSH 免密 push。

### 3. 启动

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

export OPENAI_API_KEY=sk-xxx
export OPENAI_BASE_URL=https://api.deepseek.com/v1
export OPENAI_MODEL=deepseek-chat

ros2 launch cs_core cloud_soul.launch.py \
  agent_name:=adam \
  repo_url:=git@github.com:your/agent-soul.git \
  repo_dir:=~/.adam/soul_repo
```

### 4. 对话

另开终端：

```bash
source /opt/ros/humble/setup.bash
python3 scripts/chat.py adam
```

```
╭──────────────────────────────────────────╮
│   Cloud-Soul Terminal Chat              │
│   Agent: adam                           │
│   Ctrl+C 退出  /help 查看帮助           │
╰──────────────────────────────────────────╯

▸ 你好
Adam [13:28:27]
  你好，Leaf。等你的任务。
```

---

## 🧠 核心机制

### 思考循环

Agent 在无限循环中执行：

```
感知 → 记忆召回 → LLM 推理 → 工具调用 → 结果回流 → 持久化
  ↑                                                      │
  └──────────────────────────────────────────────────────┘
```

- **有用户指令时**：立即处理并回复
- **无指令时**：根据自我认知自主决策（可选）

### 分层记忆

记忆托管在 Git 仓库，跨终端自动同步：

| 文件 | 用途 |
|---|---|
| `prompts/RULE.md` | 系统提示词（含 `[cognitions/*.md]` 占位符，运行时自动展开） |
| `cognitions/SELF.md` | Agent 自我认知 |
| `cognitions/MASTER.md` | 使用者档案 |
| `cognitions/METHOD.md` | 行为准则（经验积累） |
| `cognitions/WORLD.md` | 世界认知（人、物、事） |
| `diaries/YYYYMMDD.md` | 每日日记（LLM 自动摘要归档） |

### 上下文压缩

超过 token 阈值时，LLM 自动将历史对话压缩为摘要存入日记，清空上下文继续运行。

---

## 🏗️ 架构

```
Cloud-Soul（ROS2 工作空间）

  cs_input               cs_core                 cs_output
  ┌────────────┐      ┌──────────────┐      ┌──────────────┐
  │ 系统状态采集│      │  Agent 主循环 │      │ shell 命令    │
  │ 用户指令订阅│ ───→ │  LLM 推理    │ ───→ │ 文件读写      │
  └────────────┘      │  Git 记忆管理│      │ 邮件通知      │
                      └──────────────┘      └──────────────┘
                            │
                      GitHub 仓库
                    （记忆/认知/日记）
```

三个 ROS2 包：`cs_input`（感知层）、`cs_core`（推理+记忆层）、`cs_output`（工具层）。所有工具通过统一的 `ExecuteTool.action` 契约通信，扩展新工具只需在 `cs_output` 中添加新节点。

### 项目结构

```
Cloud-Soul/
├── scripts/chat.py          # 终端对话客户端
├── chat.sh                  # 一键启动聊天
├── cs_core/                 # 核心：推理 + 记忆管理
│   └── src/
│       ├── agent_loop_node.cpp   # 主循环
│       ├── mrymt_node.cpp        # Git 记忆同步
│       └── call_openai.cpp       # OpenAI API 客户端
├── cs_input/                # 感知层
│   └── src/
│       ├── input_mgmt.cpp        # 快照汇总
│       ├── system_status.cpp     # CPU/内存/磁盘/GPU 采集
│       └── user_command.cpp      # 用户指令订阅
├── cs_output/               # 工具层
│   └── src/
│       ├── output_mgmt.cpp       # 工具路由
│       ├── shell_exec.cpp        # shell 执行
│       ├── file_read.cpp         # 文件读取
│       ├── file_write.cpp        # 文件写入
│       └── user_notify.cpp       # 邮件通知
└── cs_interfaces/           # 接口定义
    ├── action/ExecuteTool.action
    └── srv/*.srv
```

---

## 🔧 配置项

| 参数 | 说明 | 默认值 |
|---|---|---|
| `agent_name` | Agent 命名空间，支持多实例 | `adam` |
| `repo_url` | 记忆仓库地址 | 必填 |
| `repo_dir` | 本地克隆路径 | `~/.{agent_name}/soul_repo` |
| `max_context_tokens` | 触发压缩的 token 阈值 | `200000` |
| `loop_rate` | 无指令时的思考频率（Hz，0=按需） | `0.0` |
| `context_dir` | 上下文持久化目录 | `~/.{agent_name}/context` |
| `openai_base_url` | OpenAI 兼容 API 地址 | 环境变量 `OPENAI_BASE_URL` |
| `openai_model` | 模型名 | 环境变量 `OPENAI_MODEL` |

---

## 🧩 自定义 Agent

1. **Fork 记忆仓库**：复制 [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul)，修改 `cognitions/` 中的文件定义 Agent 人格和规则
2. **添加工具**：在 `cs_output/src/` 中参照现有节点创建新工具，修改 `output_mgmt.cpp` 注册
3. **添加输入**：在 `cs_input/src/` 中添加新的传感器节点

---

## 📖 参考实例

| Agent | 描述 | 认知仓库 |
|---|---|---|
| [Adam](https://github.com/hachi-leaf/Adam-Soul) | Cloud-Soul 参考实现，DeepSeek 驱动 | 机器人开发助手，会日语、RAP、做饭 |

---

## 📄 License

MIT © [hachi-leaf](https://github.com/hachi-leaf)
