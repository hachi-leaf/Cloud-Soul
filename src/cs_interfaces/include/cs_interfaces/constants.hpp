// Copyright (c) leaf
// SPDX-License-Identifier: MIT
// =============================================================================
// 统一常量定义 — Cloud-Soul 全局
// 消除项目中的魔法数字和魔法文本，所有超时、重试间隔、缓冲区大小、
// 错误码、错误消息集中管理。
// =============================================================================

#ifndef CS_CORE__CONSTANTS_HPP_
#define CS_CORE__CONSTANTS_HPP_

#include <chrono>

namespace cloud_soul {

// ─────────────────────────────────────────────────────────────
// 超时与重试间隔
// ─────────────────────────────────────────────────────────────

// agent_loop — 服务等待
constexpr auto SERVICE_WAIT_TIMEOUT       = std::chrono::seconds(2);
constexpr auto SERVICE_RETRY_INTERVAL     = std::chrono::milliseconds(500);
constexpr auto SERVICE_CALL_TIMEOUT       = std::chrono::seconds(5);

// agent_loop — Action / LLM
constexpr auto ACTION_REQUEST_TIMEOUT     = std::chrono::seconds(5);
constexpr auto ACTION_RETRY_INTERVAL      = std::chrono::seconds(2);

// agent_loop — 工具调用重试 (Action Server 未就绪 / 超时时)
constexpr int  TOOL_RETRY_MAX             = 3;
constexpr auto TOOL_RETRY_INTERVAL        = std::chrono::seconds(1);

// agent_loop — 压缩
constexpr auto COMPRESS_SERVICE_TIMEOUT   = std::chrono::seconds(180);
constexpr auto COMPRESS_RETRY_INTERVAL    = std::chrono::seconds(2);

// agent_loop — 主循环空闲
constexpr auto LOOP_IDLE_SLEEP            = std::chrono::seconds(1);

// shell_exec — 进程轮询
constexpr auto PROCESS_POLL_INTERVAL      = std::chrono::milliseconds(10);

// input_mgmt — 僵尸源清理
constexpr auto CLEANUP_INTERVAL           = std::chrono::seconds(2);

// ─────────────────────────────────────────────────────────────
// 缓冲区 / 限制
// ─────────────────────────────────────────────────────────────
constexpr int TIMESTAMP_BUF_SIZE          = 32;   // 时间戳格式化 buf 长度
constexpr int DATE_BUF_SIZE               = 16;   // YYYYMMDD 日期 buf 长度
constexpr int TOKEN_ESTIMATE_DIVISOR      = 4;    // 粗略估算: 字符数 / 4 ≈ token 数
constexpr int MAX_TOOL_CONTENT_LEN        = 2000; // 工具输出截断长度 (压缩时)
constexpr int FILE_INCLUDE_MAX_DEPTH      = 20;   // [path] 占位符递归展开上限
constexpr int SHELL_OUTPUT_BUF_SIZE       = 256;  // shell_exec read() 缓冲区
constexpr int ARCHIVE_PUSH_MAX_RETRIES    = 5;    // 日记推送失败最大重试次数

// ─────────────────────────────────────────────────────────────
// 路径常量
// ─────────────────────────────────────────────────────────────
constexpr const char* COMPRESS_PROMPT_REL_PATH = "prompts/COMPRESS.md";
constexpr const char* DIARIES_REL_DIR           = "diaries";
constexpr const char* REFS_REMOTES_PREFIX       = "refs/remotes/";
constexpr const char* REFS_HEADS_PREFIX         = "refs/heads/";
constexpr const char* SHELL_TEMP_TEMPLATE       = "/tmp/cloudsoul_shell_exec_XXXXXX";
constexpr const char* SHELL_SHEBANG_LINE        = "#!/bin/sh\n";

// ─────────────────────────────────────────────────────────────
// 错误码 — 按节点分块，值与原始代码保持一致，仅消除硬编码
// ─────────────────────────────────────────────────────────────

namespace Err {

// -- memory_recall 服务 --
namespace MemRecall {
  constexpr int OK              =  0;
  constexpr int SYNC_FAILED     = -1;
  constexpr int RULE_OPEN_FAILED = -2;
}

// -- memory_archive 服务 --
namespace MemArchive {
  constexpr int OK                     =  0;
  constexpr int FILE_NOT_FOUND         = -1;
  constexpr int RENAME_FAILED          = -2;
  constexpr int COMPRESS_OPEN_FAILED   = -3;
  constexpr int PROCESSING_OPEN_FAILED = -4;
  constexpr int LLM_FAILED             = -5;
  constexpr int DIARY_DIR_FAILED       = -6;
  constexpr int DIARY_OPEN_FAILED      = -7;
  constexpr int GIT_OPEN_FAILED        = -8;
  constexpr int GIT_INDEX_FAILED       = -9;
  constexpr int GIT_ADD_FAILED         = -10;
  constexpr int GIT_IDX_WRITE_FAILED   = -11;
  constexpr int GIT_TREE_WRITE_FAILED  = -12;
  constexpr int GIT_TREE_LOOKUP_FAILED = -13;
  constexpr int GIT_COMMIT_FAILED      = -14;
  constexpr int GIT_REMOTE_FAILED      = -15;
  constexpr int GIT_PUSH_FAILED        = -16;
  constexpr int PUSH_RETRY_EXHAUSTED   = -17;
  constexpr int DONE_RENAME_FAILED     = -18;
}

// -- message_send_node --
namespace MsgSend {
  constexpr int OK               =  0;
  constexpr int INVALID_INPUT    = -1;
  constexpr int UNSUPPORTED_CHAN = -2;
  constexpr int SNAIL_UNAVAIL    = -3;
}

// -- shell_exec_node --
namespace ShellExec {
  constexpr int OK               =  0;
  constexpr int INVALID_INPUT    = -1;
  constexpr int TEMP_FILE_FAIL   = -2;
  constexpr int TEMP_WRITE_FAIL  = -3;
  constexpr int PIPE_FAIL        = -4;
  constexpr int FORK_FAIL        = -5;
  constexpr int CANCELED         = -7;
  constexpr int NONZERO_EXIT     = -8;
}

}  // namespace Err

// ─────────────────────────────────────────────────────────────
// 错误消息 / 日志文本 — 语义化的中文 / 英文常量
// ─────────────────────────────────────────────────────────────

namespace Msg {

// --- agent_loop_node ---
constexpr const char* ACTION_SRV_NOT_READY  = "动作服务器未就绪";
constexpr const char* TOOL_CALL_TIMEOUT     = "工具调用超时";
constexpr const char* TOOL_GOAL_REJECTED    = "目标被拒绝";
constexpr const char* TOOL_EXEC_TIMEOUT     = "工具执行超时";
constexpr const char* TOOL_EXEC_FAILED      = "工具执行失败";
constexpr const char* TOOL_OUTPUT_INVALID   = "工具输出 JSON 无效";
constexpr const char* TOOL_PARAM_INVALID    = "工具参数 JSON 无效: ";
constexpr const char* OUTPUT_ACTION_NOT_RDY = "输出动作服务器未就绪，将在后续调用时重试";
constexpr const char* INCOMPLETE_TOOL_FOUND = "发现未完成的工具调用，将重新执行";
constexpr const char* RECOVERED_TOOL_CALL   = "已恢复未完成的工具调用";
constexpr const char* CONTEXT_PARSE_FAILED  = "上下文文件解析失败，将创建新上下文";
constexpr const char* NO_CHANNEL_REMINDER   = "本Agent架构请勿 reply，每次输出必须调用工具，如需通知用户请使用 message_send 工具，如需静默请使用 shell_exec 执行 sleep 10";
constexpr const char* TOOL_OUTPUT_TRUNCATED = "\n...[工具输出已截断]";
constexpr const char* COMPRESSION_REMINDER  = "记忆压缩完成，请基于当前系统提示词和对话摘要，继续为用户提供帮助。";
constexpr const char* TOOL_RETRY_MSG        = "工具调用失败，第 %d/%d 次重试...";

// --- memory_node ---
constexpr const char* REPO_SYNC_FAILED       = "仓库同步失败";
constexpr const char* RULE_OPEN_FAILED       = "无法打开规则文件: ";
constexpr const char* REPO_OPEN_FAILED       = "打开仓库失败";
constexpr const char* LLM_FAILED             = "大模型调用失败或返回格式错误";
constexpr const char* DIARY_OPEN_FAILED      = "打开日记文件失败";
constexpr const char* PUSH_SUCCESS           = "推送成功";
constexpr const char* PUSH_FAILED_NO_RETRY   = "推送失败，不再重试";

// --- message_send_node (英文 JSON 返回串) ---
constexpr const char* JSON_INVALID_INPUT     = "{\"error\":\"invalid input json or missing channel\"}";
constexpr const char* JSON_UNSUPPORTED_CHAN  = "{\"error\":\"unsupported channel\"}";
constexpr const char* JSON_SNAIL_NOT_INST    = "{\"error\":\"s-nail not installed\"}";
constexpr const char* JSON_SNAIL_SEND_FAIL   = "{\"error\":\"s-nail send failed\"}";
constexpr const char* JSON_INVALID_EMAIL     = "{\"error\":\"invalid email parameters\"}";
constexpr const char* JSON_INVALID_TOPIC     = "{\"error\":\"invalid topic parameters\"}";
constexpr const char* JSON_INVALID_WEBCHAT   = "{\"error\":\"invalid web_chat parameters\"}";
constexpr const char* JSON_STATUS_SENT       = "{\"status\":\"sent\"}";
constexpr const char* JSON_STATUS_PUBLISHED  = "{\"status\":\"published\"}";

// --- shell_exec_node (英文 JSON 返回串) ---
constexpr const char* SHELL_JSON_INVALID_INPUT  = "{\"error\":\"invalid input json or missing command\"}";
constexpr const char* SHELL_JSON_TEMP_FAIL      = "{\"error\":\"failed to create temp file\"}";
constexpr const char* SHELL_JSON_WRITE_FAIL     = "{\"error\":\"failed to write temp script\"}";
constexpr const char* SHELL_JSON_PIPE_FAIL      = "{\"error\":\"pipe creation failed\"}";
constexpr const char* SHELL_JSON_FORK_FAIL      = "{\"error\":\"fork failed\"}";
constexpr const char* SHELL_JSON_CANCELED_PFX   = "{\"error\":\"execution canceled\",\"stdout\":\"";

}  // namespace Msg

}  // namespace cloud_soul

#endif  // CS_CORE__CONSTANTS_HPP_
