#!/usr/bin/env bash
AGENT="${AGENT_NAME:-adam}"
MESSAGE="$*"
if [ -z "$MESSAGE" ]; then echo "用法: $0 <消息内容>"; exit 1; fi
ros2 service call /${AGENT}/input/message_receive/ros2_msg cs_interfaces/srv/SendMessage "{message: '${MESSAGE}'}"