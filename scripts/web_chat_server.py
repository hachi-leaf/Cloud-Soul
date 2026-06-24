#!/usr/bin/env python3
"""
web_chat_server.py v3 — DeepSeek 风格双栏布局
新增: /sessions → 对话历史列表 (用于侧边栏)
"""

import os, sys, json, argparse, threading, time, signal, atexit, uuid
from pathlib import Path
from flask import Flask, request, jsonify, Response, send_from_directory

import rclpy
from rclpy.node import Node
from std_msgs.msg import String
from cs_interfaces.srv import SendMessage

parser = argparse.ArgumentParser(description='Web Chat Server v3')
parser.add_argument('--agent', default='agent', help='Agent 名称')
parser.add_argument('--port', type=int, default=8080, help='监听端口')
args = parser.parse_args()
AGENT_NAME = args.agent
PORT = args.port

app = Flask(__name__)
app.config['MAX_CONTENT_LENGTH'] = 10 * 1024 * 1024 * 1024
UPLOAD_DIR = Path('/tmp/web_uploads')
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)

SCRIPT_DIR = Path(__file__).parent.resolve()
AVATAR_DIR = SCRIPT_DIR / 'config' / 'avatar'
AVATAR_DIR.mkdir(parents=True, exist_ok=True)

# 历史 & 会话管理
_history: list = []
_sessions: list = []  # [{id, title, date, last_msg}]
_history_max = 200
_seq_counter = [0]
_history_lock = threading.Lock()
_current_session_id = str(uuid.uuid4())[:8]

def broadcast_message(msg: str, role='agent', files=None):
    with _history_lock:
        _seq_counter[0] += 1
        entry = {'seq': _seq_counter[0], 'role': role, 'msg': msg,
                 'ts': time.strftime('%H:%M:%S'),
                 'session': _current_session_id}
        if files:
            entry['files'] = files
        _history.append(entry)
        # 更新当前 session
        for s in _sessions:
            if s['id'] == _current_session_id:
                s['last_msg'] = msg[:60]
                s['date'] = time.strftime('%Y-%m-%d')
                break
        else:
            _sessions.append({
                'id': _current_session_id,
                'title': msg[:30],
                'date': time.strftime('%Y-%m-%d'),
                'last_msg': msg[:60]
            })
        if len(_history) > _history_max:
            _history.pop(0)

class WebChatNode(Node):
    def __init__(self, agent_name):
        super().__init__('web_chat_server_node')
        self.agent_name = agent_name
        self.send_cli = self.create_client(
            SendMessage, f'/{agent_name}/input/message_receive/web_chat')
        while not self.send_cli.wait_for_service(timeout_sec=5.0):
            self.get_logger().warn(f'等待服务 /{agent_name}/input/message_receive/web_chat ...')
        self.get_logger().info(f'web_chat 服务已连接 -> /{agent_name}')
        self.sub = self.create_subscription(
            String, f'/{agent_name}/output/message_send/web_chat',
            self.on_agent_reply, 10)

    def on_agent_reply(self, msg):
        broadcast_message(msg.data, role='agent')

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

ros_node = None

def ros_spin():
    global ros_node
    rclpy.init()
    ros_node = WebChatNode(AGENT_NAME)
    rclpy.spin(ros_node)
    ros_node.destroy_node()
    rclpy.shutdown()

# Flask routes
@app.route('/')
def index():
    html_path = SCRIPT_DIR / 'chat.html'
    html = html_path.read_text(encoding='utf-8') if html_path.exists() else '<h1>chat.html not found</h1>'
    html = html.replace('{agent_name}', AGENT_NAME)
    resp = app.response_class(response=html, status=200, mimetype='text/html')
    resp.headers['Cache-Control'] = 'no-cache'
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
    display_msg = msg
    if files:
        parts = [f"{f.get('original_name', f.get('name', 'unknown'))} ({f.get('size_human', '?')})" for f in files]
        display_msg = msg + '\n[附件: ' + ', '.join(parts) + ']'
        msg = msg + '\n[附件: ' + ', '.join(parts) + ']'
    broadcast_message(display_msg, role='user', files=data.get('files'))
    result = ros_node.send_to_agent(msg)
    return jsonify(result)

@app.route('/upload', methods=['POST'])
def handle_upload():
    if 'files' not in request.files:
        return jsonify({'success': False, 'error': 'missing files'}), 400
    files = request.files.getlist('files')
    results = []
    for f in files:
        if not f.filename: continue
        p = Path(f.filename)
        uid = uuid.uuid4().hex[:8]
        safe_name = f"{p.stem[:80]}_{uid}{p.suffix[:16]}".replace('/', '_')
        save_path = UPLOAD_DIR / safe_name
        try:
            f.save(str(save_path))
            sz = save_path.stat().st_size
            results.append({'original_name': f.filename, 'saved_name': safe_name,
                          'path': str(save_path), 'size': sz, 'size_human': format_size(sz)})
        except Exception as e:
            results.append({'original_name': f.filename, 'error': str(e)})
    return jsonify({'success': True, 'files': results})

@app.route('/history')
def history():
    with _history_lock:
        return jsonify({'messages': list(_history), 'last_seq': _seq_counter[0]})

@app.route('/poll')
def poll():
    after = request.args.get('after', 0, type=int)
    with _history_lock:
        new_msgs = [m for m in _history if m['seq'] > after]
    return jsonify({'messages': new_msgs, 'last_seq': _seq_counter[0]})

@app.route('/sessions')
def sessions():
    with _history_lock:
        return jsonify({'sessions': list(_sessions), 'current': _current_session_id})

@app.route('/config/avatar/<path:filename>')
def serve_avatar(filename):
    return send_from_directory(str(AVATAR_DIR), filename)

def format_size(size):
    for unit in ['B', 'KB', 'MB', 'GB']:
        if size < 1024:
            return f"{size:.1f} {unit}" if unit != 'B' else f"{size} B"
        size /= 1024
    return f"{size:.1f} TB"

if __name__ == '__main__':
    ros_thread = threading.Thread(target=ros_spin, daemon=True)
    ros_thread.start()
    time.sleep(2.0)
    signal.signal(signal.SIGTERM, lambda sig, frame: sys.exit(0))
    atexit.register(lambda: None)
    for name in ['user.png', 'agent.png']:
        p = AVATAR_DIR / name
        print(f'  头像 {name}: {"✓" if p.exists() else "✗"}')
    print(f'  {AGENT_NAME} Web Chat v3 (DeepSeek-style)')
    print(f'  http://localhost:{PORT}')
    app.run(host='0.0.0.0', port=PORT, debug=False, threaded=True)