#!/usr/bin/env python3
"""
web_chat_server.py - Web 聊天独立服务器

通过 Flask + rclpy 桥接浏览器与 Agent：
- 用户浏览器 POST /send → ROS2 服务 /<agent>/input/message_receive/web_chat
- Agent 回复 → ROS2 话题 /<agent>/output/message_send/web_chat → SSE 推送浏览器

用法:
    python3 web_chat_server.py [--agent <agent_name>] [--port <port>]
    默认: agent=agent_test, port=8080
"""

import os
import sys
import json
import argparse
import threading
import queue
import time

from flask import Flask, request, jsonify, Response

import rclpy
from rclpy.node import Node
from std_msgs.msg import String
from cs_interfaces.srv import SendMessage


# ------- 命令行参数 -------
parser = argparse.ArgumentParser(description='Web Chat Server')
parser.add_argument('--agent', default='agent_test', help='Agent 名称 (默认: agent_test)')
parser.add_argument('--port', type=int, default=8080, help='监听端口 (默认: 8080)')
args = parser.parse_args()

AGENT_NAME = args.agent
PORT = args.port

app = Flask(__name__)
message_queue = queue.Queue(maxsize=256)


# ------- HTML (DeepSeek 风格) -------
def make_html(agent_name):
    html_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chat.html')
    if os.path.exists(html_path):
        with open(html_path, 'r', encoding='utf-8') as f:
            return f.read().replace('{agent_name}', agent_name)
    return f"<h1>{agent_name} Chat</h1><p>chat.html not found</p>"




# ------- ROS2 节点 -------
class WebChatNode(Node):
    def __init__(self, agent_name):
        super().__init__('web_chat_server_node')
        self.agent_name = agent_name

        self.send_cli = self.create_client(
            SendMessage,
            f'/{agent_name}/input/message_receive/web_chat'
        )
        while not self.send_cli.wait_for_service(timeout_sec=5.0):
            self.get_logger().warn(f'等待服务 /{agent_name}/input/message_receive/web_chat ...')
        self.get_logger().info(f'web_chat 服务已连接 -> /{agent_name}')

        self.sub = self.create_subscription(
            String,
            f'/{agent_name}/output/message_send/web_chat',
            self.on_agent_reply,
            10
        )
        self.get_logger().info(f'已订阅 /{agent_name}/output/message_send/web_chat')

    def on_agent_reply(self, msg):
        try:
            message_queue.put_nowait(msg.data)
        except queue.Full:
            self.get_logger().warn('消息队列已满，丢弃一条')

    def send_to_agent(self, message):
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
    global ros_node
    rclpy.init()
    ros_node = WebChatNode(AGENT_NAME)
    rclpy.spin(ros_node)
    ros_node.destroy_node()
    rclpy.shutdown()


# ------- Flask 路由 -------
@app.route('/')
def index():
    return make_html(AGENT_NAME)


@app.route('/send', methods=['POST'])
def handle_send():
    data = request.get_json()
    if not data or 'message' not in data:
        return jsonify({'success': False, 'error': '缺少 message 字段'}), 400
    if ros_node is None:
        return jsonify({'success': False, 'error': 'ROS2 节点未就绪'}), 503
    result = ros_node.send_to_agent(data['message'])
    return jsonify(result)


@app.route('/stream')
def stream():
    def generate():
        while True:
            try:
                msg = message_queue.get(timeout=30)
                for line in msg.split('\n'):
                    yield f'data: {line}\n'
                yield '\n'
            except queue.Empty:
                yield ': keepalive\n\n'
    return Response(generate(), mimetype='text/event-stream',
                    headers={'Cache-Control': 'no-cache',
                             'X-Accel-Buffering': 'no'})


if __name__ == '__main__':
    ros_thread = threading.Thread(target=ros_spin, daemon=True)
    ros_thread.start()
    time.sleep(2.0)

    print('═══════════════════════════════════════════')
    print(f'  {AGENT_NAME} Web Chat 服务器已启动')
    print(f'  浏览器打开: http://localhost:{PORT}')
    print('═══════════════════════════════════════════')
    app.run(host='0.0.0.0', port=PORT, debug=False, threaded=True)