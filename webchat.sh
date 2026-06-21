#!/bin/bash
# Cloud-Soul Web Chat 快捷脚本
# 用法: ./webchat.sh [agent_name] [port]

AGENT="${1:-adam}"
PORT="${2:-8080}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source /opt/ros/humble/setup.bash
/usr/bin/python3 "$SCRIPT_DIR/web_chat/server.py" "$AGENT" "$PORT"
