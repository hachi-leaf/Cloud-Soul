# Cloud-Soul

基于 ROS2 的 AI Agent 运行时框架。将 LLM 推理、工具调用、记忆管理与 Git 版本控制结合，实现可多终端部署、云记忆同步的智能体运行环境。

## 解决的问题

将 AI Agent 从单次对话升级为持续运行的服务：LLM 作为思考核心，ROS2 节点作为感知和行动层，Git 仓库作为跨终端记忆载体。重启不丢失上下文，多终端共享同一份记忆。

## 架构

cs_core/        核心：agent_loop_node、mrymt_node、call_openai
cs_input/       感知：input_mgmt、system_status、user_command
cs_output/      行动：output_mgmt、shell_exec、file_read/write、user_notify
cs_interfaces/  契约：ExecuteTool.action + 4 个 service 定义

节点以 agent_name 参数做命名空间隔离，单机可运行多个 Agent 实例。

## 工作流

1. 感知 - 采集系统状态与用户指令，生成输入快照
2. 记忆召回 - 从 Git 仓库拉取 RULE.md，递归展开占位符
3. 思考行动循环 - LLM 推理，按需调用工具，结果回流上下文
4. 压缩归档 - 超 token 阈值时 LLM 压缩对话，日记追加到 Git 仓库
5. 持久化恢复 - 上下文 JSON 文件，支持断点重启

## 快速开始

依赖: ROS2 Humble, libgit2, libcurl, nlohmann-json3

```bash
cd Cloud-Soul
colcon build
source install/setup.bash
export OPENAI_API_KEY=sk-xxx
ros2 launch cs_core cloud_soul.launch.py agent_name:=adam
```

## 实例

Adam (github.com/hachi-leaf/Adam-Soul) 是本框架的运行实例，记忆托管在 Adam-Soul 仓库。
