# Cloud-Soul

[![ROS2](https://img.shields.io/badge/ROS2-Humble-blue)](https://docs.ros.org/en/humble/)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Ubuntu%2022.04-orange)]()

基于 ROS2 的 AI Agent 运行时框架。将 LLM 推理、工具调用与 Git 记忆管理结合，实现可多终端部署、云记忆同步的自主智能体。

---

## 特性

| 特性 | 说明 |
|------|------|
| 循环思考 | LLM 驱动的主循环：感知 → 推理 → 行动 → 持久化 |
| 云记忆 | Git 仓库托管提示词/认知/日记，跨终端自动同步 |
| 工具调用 | shell 执行、文件读写、邮件通知，可按需扩展 |
| 上下文压缩 | 超 token 阈值自动归档，LLM 摘要 → Git push |
| 断点恢复 | JSON 持久化上下文，重启不丢失对话 |
| 多实例 | agent_name 命名空间隔离，单机运行多个 Agent |
| 模型无关 | OpenAI 兼容 API，DeepSeek / GPT / 本地模型均可 |
| 终端对话 | `scripts/chat.py` 一行命令即可在终端与 Agent 对话 |

---

## 安装

### 1. 系统依赖

```bash
# ROS2 Humble (Ubuntu 22.04)
sudo apt install ros-humble-desktop python3-colcon-common-extensions

# 编译与运行时依赖
sudo apt install libgit2-dev libcurl4-openssl-dev nlohmann-json3-dev
```

### 2. 克隆与构建

```bash
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

### 3. 准备记忆仓库

Fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul) 作为记忆仓库（或自行创建），配置 SSH 免密 push。

---

## 运行

### 启动 Agent

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash
export OPENAI_API_KEY=sk-xxx
export OPENAI_BASE_URL=https://api.deepseek.com   # 可选，默认 OpenAI
export OPENAI_MODEL=deepseek-chat                  # 可选

ros2 launch cs_core cloud_soul.launch.py \
  agent_name:=adam \
  repo_url:=git@github.com:your/agent-soul.git \
  repo_dir:=~/.adam/soul_repo
```

### 终端对话

Agent 启动后，另开终端：

```bash
source /opt/ros/humble/setup.bash
python3 scripts/chat.py adam
```

输入文字即可与 Agent 对话。Agent 的回复通过 ROS2 topic `/adam/response` 推送，chat.py 订阅并打印。

---

## 架构

```
                    Cloud-Soul

  cs_input                 cs_core                 cs_output
  ┌──────────┐          ┌──────────────┐          ┌──────────┐
  │ system   │ snapshot │ agent_loop   │ tools     │ shell    │
  │  status  ├─────────→│  ┌────────┐ ├──────────→│  _exec   │
  │ user     │          │  │  LLM   │ │ result    │ file     │
  │  command │          │  │ think  │ │←──────────│  _read   │
  └──────────┘          │  └───┬────┘ │           │ file     │
                        │      │      │           │  _write  │
                        │  ┌───┴────┐ │           │ user     │
                        │  │ mrymt  │ │           │  _notify │
                        │  │(Git)   │ │           └──────────┘
                        │  └───────┘ │
                        └──────────────┘

  记忆仓库 (GitHub) ←──→ mrymt_node: pull RULE.md / push diaries
```

## 工作流

| 阶段 | 描述 |
|------|------|
| 感知 | system_status + user_command → GetInputSnapshot 快照 |
| 记忆召回 | mrymt_node 拉取 Git 仓库 RULE.md，展开 `[cognitions/*.md]` |
| 思考行动 | LLM 推理 → tool_calls → ExecuteTool action → 结果回流 |
| 压缩归档 | token 超限 → LLM 压缩 → diaries/YYYYMMDD.md → git push |
| 持久化 | 上下文 JSON 文件，重启恢复 |

---

## 记忆仓库结构

```
agent-soul/
  prompts/
    RULE.md          # 系统提示词（含 [cognitions/*.md] 占位符）
    COMPRESS.md      # 压缩提示词
  cognitions/
    SELF.md          # Agent 自我认知
    MASTER.md        # 使用者档案
    METHOD.md        # 行为准则
    WORLD.md         # 世界认知
  diaries/
    YYYYMMDD.md      # LLM 自动生成的每日日记
```

RULE.md 中的 `[cognitions/*.md]` 会在运行时自动展开为对应文件内容，无需手动拼接。

---

## 项目结构

```
Cloud-Soul/
  scripts/
    chat.py                 # 终端对话客户端
  cs_core/
    src/
      agent_loop_node.cpp   # 感知→推理→行动主循环
      mrymt_node.cpp        # Git 记忆拉取/日记 push
      call_openai.cpp       # libcurl OpenAI 客户端
  cs_input/
    src/
      input_mgmt.cpp        # 快照汇总
      system_status.cpp     # CPU/内存/GPU 采集
      user_command.cpp      # 用户指令订阅
  cs_output/
    src/
      output_mgmt.cpp       # 工具节点发现与路由
      shell_exec.cpp        # shell 命令执行
      file_read.cpp         # 文件读取
      file_write.cpp        # 文件写入
      user_notify.cpp       # 邮件通知
  cs_interfaces/
    action/ExecuteTool.action   # 工具调用契约
    srv/*.srv                   # 输入/记忆服务定义
```

---

## 参考实例

| Agent | 描述 | 记忆仓库 |
|-------|------|----------|
| Adam | Cloud-Soul 参考实现，DeepSeek 驱动 | [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul) |

---

## License

MIT © leaf
