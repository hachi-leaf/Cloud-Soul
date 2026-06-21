#!/bin/bash
# Cloud-Soul 终端对话快捷脚本
# 用法: ./chat.sh [agent_name]

AGENT="${1:-adam}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

source /opt/ros/humble/setup.bash
/usr/bin/python3 "$SCRIPT_DIR/scripts/chat.py" "$AGENT"
