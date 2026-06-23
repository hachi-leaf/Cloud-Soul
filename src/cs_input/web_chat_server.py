#!/usr/bin/env python3
"""
web_chat_server.py - Web 聊天独立服务器

通过 Flask + rclpy 桥接浏览器与 Adam Agent：
- 用户浏览器 POST /send → ROS2 服务 /adam/input/message_receive/web_chat
- Adam 回复 → ROS2 话题 /adam/output/message_send/web_chat → SSE 推送浏览器

启动方式:
    python3 web_chat_server.py
    # 浏览器打开 http://localhost:8080
"""

import os
import json
import threading
import queue
import time

from flask import Flask, request, jsonify, Response

import rclpy
from rclpy.node import Node
from std_msgs.msg import String
from cs_interfaces.srv import SendMessage


app = Flask(__name__)

# 消息队列：ROS2 话题消息 → SSE
message_queue = queue.Queue(maxsize=256)

# HTML 聊天界面
HTML_PAGE = """<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Adam Chat</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: monospace; background: #1a1a2e; color: #e0e0e0; height: 100vh; display: flex; flex-direction: column; }
#header { background: #16213e; padding: 12px 20px; font-size: 14px; border-bottom: 1px solid #0f3460; }
#header h1 { font-size: 18px; color: #e94560; }
#messages { flex: 1; overflow-y: auto; padding: 16px; display: flex; flex-direction: column; gap: 8px; }
.msg { max-width: 80%; padding: 10px 14px; border-radius: 8px; line-height: 1.5; white-space: pre-wrap; word-break: break-word; }
.msg.user { align-self: flex-end; background: #0f3460; color: #e0e0e0; }
.msg.adam { align-self: flex-start; background: #16213e; color: #c0c0c0; border: 1px solid #0f3460; }
.msg .time { font-size: 10px; color: #888; margin-bottom: 4px; }
#input-area { display: flex; padding: 12px; background: #16213e; border-top: 1px solid #0f3460; }
#input-area input { flex: 1; padding: 10px 14px; border: 1px solid #0f3460; border-radius: 6px; background: #1a1a2e; color: #e0e0e0; font-family: monospace; font-size: 14px; outline: none; }
#input-area input:focus { border-color: #e94560; }
#input-area button { margin-left: 8px; padding: 10px 20px; background: #e94560; color: #fff; border: none; border-radius: 6px; cursor: pointer; font-family: monospace; font-size: 14px; }
#input-area button:hover { background: #c73e54; }
#status { font-size: 11px; color: #666; text-align: center; padding: 6px; }
</style>
</head>
<body>
<div id="header"><h1>🤖 Adam Chat</h1></div>
<div id="messages"></div>
<div id="input-area">
    <input id="msg-input" type="text" placeholder="输入消息..." autofocus>
    <button id="send-btn">发送</button>
</div>
<div id="status">已连接</div>

<script>
const msgsEl = document.getElementById('messages');
const inputEl = document.getElementById('msg-input');
const sendBtn = document.getElementById('send-btn');
const statusEl = document.getElementById('status');

function addMessage(text, role) {
    const div = document.createElement('div');
    div.className = 'msg ' + role;
    const now = new Date();
    const ts = now.toTimeString().slice(0, 8);
    div.innerHTML = '<div class="time">' + ts + '</div>' + text;
    msgsEl.appendChild(div);
    msgsEl.scrollTop = msgsEl.scrollHeight;
}

async function sendMessage() {
    const text = inputEl.value.trim();
    if (!text) return;
    addMessage(text, 'user');
    inputEl.value = '';
    statusEl.textContent = '发送中...';

    try {
        const res = await fetch('/send', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ message: text })
        });
        const data = await res.json();
        if (data.success) {
            statusEl.textContent = '已发送';
        } else {
            statusEl.textContent = '发送失败: ' + (data.error || 'unknown');
        }
    } catch (e) {
        statusEl.textContent = '连接错误: ' + e.message;
    }
}

sendBtn.addEventListener('click', sendMessage);
inputEl.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') sendMessage();
});

// SSE 接收 Adam 回复
const evtSource = new EventSource('/stream');
evtSource.onmessage = (event) => {
    addMessage(event.data, 'adam');
    statusEl.textContent = '已连接';
};
evtSource.onerror = () => {
    statusEl.textContent = '连接断开，正在重连...';
};
</script>
</body>
</html>"""


class WebChatNode(Node):
    """ROS2 节点：桥接 Flask 与 Adam"""

    def __init__(self):
        super().__init__('web_chat_server_node')

        agent_name = 'adam'

        # 客户端：调用 Adam 的消息接收服务
        self.send_cli = self.create_client(
            SendMessage,
            f'/{agent_name}/input/message_receive/web_chat'
        )
        while not self.send_cli.wait_for_service(timeout_sec=5.0):
            self.get_logger().warn('等待服务 /adam/input/message_receive/web_chat ...')
        self.get_logger().info('web_chat 服务已连接')

        # 订阅 Adam 的 web_chat 输出话题
        self.sub = self.create_subscription(
            String,
            f'/{agent_name}/output/message_send/web_chat',
            self.on_adam_reply,
            10
        )
        self.get_logger().info('已订阅 /adam/output/message_send/web_chat')

    def on_adam_reply(self, msg):
        """Adam 回复到达，放入队列供 SSE 消费"""
        try:
            message_queue.put_nowait(msg.data)
        except queue.Full:
            self.get_logger().warn('消息队列已满，丢弃一条消息')

    def send_to_adam(self, message):
        """同步调用 ROS2 服务发送消息给 Adam"""
        req = SendMessage.Request()
        req.message = message
        future = self.send_cli.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=5.0)
        if future.done() and future.result() is not None:
            resp = future.result()
            return {'success': resp.success, 'message': resp.message}
        else:
            return {'success': False, 'error': 'ROS2 服务超时'}


# ------- ROS2 线程 -------
ros_node = None

def ros_spin():
    """在独立线程中运行 rclpy.spin"""
    global ros_node
    rclpy.init()
    ros_node = WebChatNode()
    rclpy.spin(ros_node)
    ros_node.destroy_node()
    rclpy.shutdown()


# ------- Flask 路由 -------

@app.route('/')
def index():
    return HTML_PAGE


@app.route('/send', methods=['POST'])
def handle_send():
    data = request.get_json()
    if not data or 'message' not in data:
        return jsonify({'success': False, 'error': '缺少 message 字段'}), 400
    if ros_node is None:
        return jsonify({'success': False, 'error': 'ROS2 节点未就绪'}), 503

    result = ros_node.send_to_adam(data['message'])
    return jsonify(result)


@app.route('/stream')
def stream():
    """SSE 端点：推送 Adam 回复"""
    def generate():
        while True:
            try:
                msg = message_queue.get(timeout=30)
                yield f'data: {msg}\n\n'
            except queue.Empty:
                yield ': keepalive\n\n'
    return Response(generate(), mimetype='text/event-stream',
                    headers={'Cache-Control': 'no-cache',
                             'X-Accel-Buffering': 'no'})


if __name__ == '__main__':
    # 启动 ROS2 线程
    ros_thread = threading.Thread(target=ros_spin, daemon=True)
    ros_thread.start()

    # 等待 ROS2 就绪
    time.sleep(2.0)

    print('═══════════════════════════════════════════')
    print('  Adam Web Chat 服务器已启动')
    print('  浏览器打开: http://localhost:8080')
    print('═══════════════════════════════════════════')
    app.run(host='0.0.0.0', port=8080, debug=False, threaded=True)