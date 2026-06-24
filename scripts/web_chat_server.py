#!/usr/bin/env python3
"""
web_chat_server.py - Web 聊天独立服务器

通过 Flask + rclpy 桥接浏览器与 Agent：
- 用户浏览器 POST /send → ROS2 服务 /<agent>/input/message_receive/web_chat
- Agent 回复 → ROS2 话题 /<agent>/output/message_send/web_chat → SSE 推送浏览器
- POST /upload → 文件上传，保存到 /tmp/web_uploads/

用法:
    python3 web_chat_server.py [--agent <agent_name>] [--port <port>]
    默认: agent=agent_test, port=8080
"""

import os
import sys
import json
import argparse
import threading
import concurrent.futures
import queue
import time

import signal
import atexit
import uuid
from pathlib import Path

from flask import Flask, request, jsonify, Response, make_response

import rclpy
from rclpy.node import Node
from std_msgs.msg import String
from cs_interfaces.srv import SendMessage


# ------- 命令行参数 -------
parser = argparse.ArgumentParser(description='Web Chat Server')
parser.add_argument('--agent', default='agent', help='Agent 名称 (默认: agent)')
parser.add_argument('--port', type=int, default=8080, help='监听端口 (默认: 8080)')
args = parser.parse_args()

AGENT_NAME = args.agent
PORT = args.port

app = Flask(__name__)
# 多连接广播：每个 SSE 连接一个独立队列，消息到来时广播到所有队列
_conns = []          # list of queue.Queue
_conns_lock = threading.Lock()
_history = []        # list of {'role': 'user'/'agent', 'msg': str}
_history_max = 200   # 最多保留条数

def broadcast_message(msg, role='agent'):
    payload = json.dumps({'role': role, 'msg': msg})
    # 记录历史
    _history.append({'role': role, 'msg': msg})
    if len(_history) > _history_max:
        _history.pop(0)
    # 广播到所有连接
    with _conns_lock:
        for q in _conns[:]:
            try:
                q.put_nowait(payload)
            except queue.Full:
                pass

app.config['MAX_CONTENT_LENGTH'] = 10 * 1024 * 1024 * 1024  # 10GB
UPLOAD_DIR = Path('/tmp/web_uploads')
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)


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
        broadcast_message(msg.data)

    def send_to_agent(self, message):
        req = SendMessage.Request()
        req.message = message
        future = self.send_cli.call_async(req)
        event = threading.Event()
        result = {'success': False, 'error': 'ROS2 服务超时'}
        def callback(f):
            if f.done() and f.result() is not None:
                resp = f.result()
                result['success'] = resp.success
                result['message'] = resp.message
                result.pop('error', None)
            event.set()
        future.add_done_callback(callback)
        if not event.wait(timeout=5.0):
            return {'success': False, 'error': 'ROS2 服务超时'}
        return result


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
    resp = app.response_class(
        response=make_html(AGENT_NAME),
        status=200,
        mimetype='text/html'
    )
    resp.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
    resp.headers['Pragma'] = 'no-cache'
    resp.headers['Expires'] = '0'
    return resp


@app.route('/send', methods=['POST'])
def handle_send():
    data = request.get_json()
    if not data or 'message' not in data:
        return jsonify({'success': False, 'error': '缺少 message 字段'}), 400
    if ros_node is None:
        return jsonify({'success': False, 'error': 'ROS2 节点未就绪'}), 503

    msg = data['message'] or ''
    files = data.get('files', [])
    display_msg = msg  # 用户可见的消息文本
    if files:
        parts = []
        for f in files:
            name = f.get('original_name') or f.get('name', 'unknown')
            size = f.get('size', 0)
            if size >= 1048576:
                sz = f'{size/1048576:.1f}MB'
            elif size >= 1024:
                sz = f'{size/1024:.1f}KB'
            else:
                sz = f'{size}B'
            parts.append(f'{name} ({sz})')
        attachment = '\n[附件: ' + ', '.join(parts) + ']'
        msg = msg + attachment
        display_msg = display_msg + attachment

    # 广播用户消息到所有窗口
    broadcast_message(display_msg, role='user')

    result = ros_node.send_to_agent(msg)
    return jsonify(result)



@app.route('/upload', methods=['POST'])
def handle_upload():
    if 'files' not in request.files:
        return jsonify({'success': False, 'error': 'missing files'}), 400
    files = request.files.getlist('files')
    if not files or all(f.filename == '' for f in files):
        return jsonify({'success': False, 'error': 'no files selected'}), 400
    results = []
    for f in files:
        if f.filename == '':
            continue
        p = Path(f.filename)
        stem = p.stem[:80]
        suffix = p.suffix[:16]
        uid = uuid.uuid4().hex[:8]
        safe_name = f"{stem}_{uid}{suffix}".replace('/', '_').replace('\\', '_')
        save_path = UPLOAD_DIR / safe_name
        try:
            f.save(str(save_path))
            sz = save_path.stat().st_size
            results.append({'original_name': f.filename, 'saved_name': safe_name, 'path': str(save_path), 'size': sz, 'size_human': format_size(sz)})
        except Exception as e:
            results.append({'original_name': f.filename, 'error': str(e)})
    return jsonify({'success': True, 'files': results})

@app.route('/history')
def history():
    return jsonify({'messages': list(_history)})

@app.route('/stream')
def stream():
    q = queue.Queue(maxsize=64)
    with _conns_lock:
        _conns.append(q)
    def generate():
        try:
            while True:
                try:
                    msg = q.get(timeout=5)
                    for line in msg.split('\n'):
                        yield f'data: {line}\n'
                    yield '\n'
                except queue.Empty:
                    yield ': keepalive\n\n'
                except GeneratorExit:
                    break
                except Exception:
                    yield ': keepalive\n\n'
        finally:
            with _conns_lock:
                if q in _conns:
                    _conns.remove(q)
    return Response(generate(), mimetype='text/event-stream',
                    headers={'Cache-Control': 'no-cache',
                             'X-Accel-Buffering': 'no'})


def format_size(size):
    for unit in ['B', 'KB', 'MB', 'GB']:
        if size < 1024:
            return f"{size:.1f} {unit}" if unit != 'B' else f"{size} B"
        size /= 1024
    return f"{size:.1f} TB"

def shutdown():
    rclpy.shutdown()
    # Flask 会自动停止

if __name__ == '__main__':
    ros_thread = threading.Thread(target=ros_spin, daemon=True)
    ros_thread.start()
    time.sleep(2.0)

    signal.signal(signal.SIGTERM, lambda sig, frame: sys.exit(0))
    atexit.register(shutdown)

    print(f'  文件上传目录: {UPLOAD_DIR}')
    print('═══════════════════════════════════════════')
    print(f'  {AGENT_NAME} Web Chat 服务器已启动')
    print(f'  浏览器打开: http://localhost:{PORT}')
    print(f'  文件上传目录: {UPLOAD_DIR}')
    print('═══════════════════════════════════════════')
    app.run(host='0.0.0.0', port=PORT, debug=False, threaded=True)