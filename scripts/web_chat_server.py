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
    return f"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{agent_name} Chat</title>
<style>
* {{ margin: 0; padding: 0; box-sizing: border-box; }}
body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; background: #f5f6f8; color: #1a1a2e; height: 100vh; display: flex; flex-direction: column; }}
/* 顶栏 */
#header {{ background: #fff; padding: 12px 24px; border-bottom: 1px solid #e5e7eb; display: flex; align-items: center; gap: 10px; flex-shrink: 0; }}
#header .logo {{ width: 32px; height: 32px; background: #4d6bfe; border-radius: 8px; display: flex; align-items: center; justify-content: center; font-size: 18px; color: #fff; }}
#header h1 {{ font-size: 15px; font-weight: 600; color: #1a1a2e; }}
#header .sub {{ font-size: 12px; color: #999; margin-left: auto; }}
/* 消息区 */
#messages {{ flex: 1; overflow-y: auto; padding: 24px; display: flex; flex-direction: column; gap: 16px; max-width: 800px; width: 100%; margin: 0 auto; }}
.msg {{ display: flex; flex-direction: column; max-width: 90%; animation: fadeIn 0.2s ease; }}
@keyframes fadeIn {{ from {{ opacity: 0; transform: translateY(4px); }} to {{ opacity: 1; transform: translateY(0); }} }}
.msg.user {{ align-self: flex-end; }}
.msg.agent {{ align-self: flex-start; }}
.msg .bubble {{ padding: 12px 16px; border-radius: 12px; line-height: 1.6; font-size: 14px; white-space: pre-wrap; word-break: break-word; }}
.msg.user .bubble {{ background: #4d6bfe; color: #fff; border-bottom-right-radius: 4px; }}
.msg.agent .bubble {{ background: #fff; color: #1a1a2e; border: 1px solid #e5e7eb; border-bottom-left-radius: 4px; }}
.msg .time {{ font-size: 11px; color: #999; margin-top: 4px; padding: 0 4px; }}
/* Markdown 样式 */
.agent .bubble pre {{ position: relative; }}
.agent .bubble pre .copy-btn {{ position: absolute; top: 6px; right: 8px; background: #3b3b5c; color: #cdd6f4; border: 1px solid #585b70; border-radius: 4px; padding: 2px 8px; font-size: 11px; cursor: pointer; opacity: 0; transition: opacity 0.2s; }}
.agent .bubble pre:hover .copy-btn {{ opacity: 1; }}
.agent .bubble pre .copy-btn.copied {{ background: #40a02b; border-color: #40a02b; }}
.agent .bubble pre {{ background: #1e1e2e; color: #cdd6f4; padding: 14px 16px; border-radius: 8px; overflow-x: auto; margin: 8px 0; font-size: 13px; line-height: 1.5; }}
.agent .bubble code {{ font-family: "JetBrains Mono", "Fira Code", monospace; font-size: 13px; }}
.agent .bubble :not(pre) > code {{ background: #e8e8f0; color: #d63384; padding: 2px 6px; border-radius: 4px; }}
.agent .bubble table {{ border-collapse: collapse; margin: 8px 0; width: 100%; }}
.agent .bubble th, .agent .bubble td {{ border: 1px solid #d1d5db; padding: 8px 12px; text-align: left; }}
.agent .bubble th {{ background: #f3f4f6; font-weight: 600; }}
.agent .bubble blockquote {{ border-left: 3px solid #4d6bfe; padding: 4px 12px; margin: 8px 0; color: #666; background: #f8f9fc; border-radius: 0 6px 6px 0; }}
.agent .bubble h1, .agent .bubble h2, .agent .bubble h3 {{ margin: 12px 0 6px; }}
.agent .bubble h1 {{ font-size: 18px; }} .agent .bubble h2 {{ font-size: 16px; }} .agent .bubble h3 {{ font-size: 14px; }}
.agent .bubble ul, .agent .bubble ol {{ padding-left: 20px; margin: 4px 0; }}
.agent .bubble hr {{ border: none; border-top: 1px solid #e5e7eb; margin: 12px 0; }}
.agent .bubble a {{ color: #4d6bfe; }}
.msg.user .time {{ text-align: right; }}
/* 欢迎消息 */
.welcome {{ text-align: center; padding: 40px 20px; color: #999; }}
.welcome .icon {{ font-size: 48px; margin-bottom: 12px; }}
.welcome h2 {{ font-size: 20px; color: #333; margin-bottom: 8px; }}
.welcome p {{ font-size: 14px; }}
/* 输入区 */
#input-area {{ background: #fff; border-top: 1px solid #e5e7eb; padding: 16px 24px; flex-shrink: 0; }}
#input-area .wrapper {{ max-width: 800px; margin: 0 auto; display: flex; gap: 10px; align-items: center; }}
#input-area textarea {{ flex: 1; padding: 12px 16px; border: 1px solid #e5e7eb; border-radius: 10px; background: #f9fafb; color: #1a1a2e; font-family: inherit; font-size: 14px; outline: none; resize: none; min-height: 44px; max-height: 150px; line-height: 1.5; transition: border-color 0.2s; }}
#input-area textarea:focus {{ border-color: #4d6bfe; background: #fff; }}
#input-area button {{ padding: 10px 20px; background: #4d6bfe; color: #fff; border: none; border-radius: 10px; cursor: pointer; font-size: 14px; font-weight: 500; white-space: nowrap; transition: background 0.2s; }}
#input-area button:hover {{ background: #3b5de7; }}
#input-area button:disabled {{ background: #c7d2fe; cursor: not-allowed; }}
#status {{ font-size: 11px; color: #bbb; text-align: center; padding: 4px; }}
/* 滚动条 */
#messages::-webkit-scrollbar {{ width: 6px; }}
#messages::-webkit-scrollbar-track {{ background: transparent; }}
#messages::-webkit-scrollbar-thumb {{ background: #d1d5db; border-radius: 3px; }}
</style>
<script>
// Minimal markdown renderer (no deps)
function renderMD(t) {{
    // fenced code blocks
    t = t.replace(/```(\\w*)\\n([\\s\\S]*?)```/g, function(_, lang, code) {{
        return '<pre><code' + (lang ? ' class="language-' + lang + '"' : '') + '>' +
            code.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;') + '</code></pre>';
    }});
    // inline code
    t = t.replace(/`([^`]+)`/g, '<code>$1</code>');
    // bold
    t = t.replace(/\\*\\*(.+?)\\*\\*/g, '<strong>$1</strong>');
    // italic
    t = t.replace(/\\*(.+?)\\*/g, '<em>$1</em>');
    // headings
    t = t.replace(/^### (.+)$/gm, '<h3>$1</h3>');
    t = t.replace(/^## (.+)$/gm, '<h2>$1</h2>');
    t = t.replace(/^# (.+)$/gm, '<h1>$1</h1>');
    // blockquote
    t = t.replace(/^&gt; (.+)$/gm, '<blockquote>$1</blockquote>');
    // hr
    t = t.replace(/^---$/gm, '<hr>');
    // tables
    t = t.replace(/^\\|(.+)\\|$/gm, function(line) {{
        var cells = line.split('|').filter(function(c) {{ return c.trim(); }});
        if (cells.length === 0) return line;
        var tag = line.match(/^\\|[\\s:\\-]+\\|$/) ? 'th' : 'td';
        return '<tr>' + cells.map(function(c) {{ return '<' + tag + '>' + c.trim() + '</' + tag + '>'; }}).join('') + '</tr>';
    }});
    t = t.replace(/(<tr>.*<\\/tr>\\n?)+/g, '<table>$&</table>');
    // lists
    t = t.replace(/^[*-] (.+)$/gm, '<li>$1</li>');
    t = t.replace(/(<li>.*<\\/li>\\n?)+/g, '<ul>$&</ul>');
    t = t.replace(/^\\d+\\. (.+)$/gm, '<li>$1</li>');
    // paragraphs
    var parts = t.split(/\\n\\n/);
    t = parts.map(function(p) {{
        p = p.trim();
        if (!p) return '';
        if (/^<(h[1-3]|table|ul|ol|pre|blockquote|hr)/.test(p)) return p;
        return '<p>' + p.replace(/\\n/g, '<br>') + '</p>';
    }}).join('');
    return t;
}}
</script>
</head>
<body>
<div id="header">
    <div class="logo">🤖</div>
    <h1>{agent_name}</h1>
    <span class="sub">web_chat</span>
</div>
<div id="messages">
    <div class="welcome">
        <div class="icon">👋</div>
        <h2>你好，我是 {agent_name}</h2>
        <p>在下方输入消息，开始对话</p>
    </div>
</div>
<div id="status">已连接</div>
<div id="input-area">
    <div class="wrapper">
        <textarea id="msg-input" placeholder="输入消息..." rows="1" autofocus></textarea>
        <button id="send-btn">发送</button>
    </div>
</div>

<script>
const msgsEl = document.getElementById('messages');
const inputEl = document.getElementById('msg-input');
const sendBtn = document.getElementById('send-btn');
const statusEl = document.getElementById('status');
let welcomeRemoved = false;

function removeWelcome() {{
    if (!welcomeRemoved) {{
        const w = msgsEl.querySelector('.welcome');
        if (w) w.remove();
        welcomeRemoved = true;
    }}
}}

function addMessage(text, role) {{
    removeWelcome();
    const div = document.createElement('div');
    div.className = 'msg ' + (role === 'user' ? 'user' : 'agent');
    const now = new Date();
    const ts = now.getHours().toString().padStart(2,'0') + ':' + now.getMinutes().toString().padStart(2,'0');
    const html = role === 'user'
        ? text.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
        : renderMD(text);
    div.innerHTML = '<div class="bubble">' + html + '</div><div class="time">' + ts + '</div>';
    msgsEl.appendChild(div);
    div.querySelectorAll('pre').forEach(function(p) {{
        if (p.querySelector('.copy-btn')) return;
        var b = document.createElement('button');
        b.className = 'copy-btn'; b.textContent = 'Copy';
        b.onclick = function() {{
            var code = p.querySelector('code') || p;
            navigator.clipboard.writeText(code.textContent).then(function() {{
                b.textContent = 'Copied!'; b.classList.add('copied');
                setTimeout(function() {{ b.textContent = 'Copy'; b.classList.remove('copied'); }}, 2000);
            }});
        }};
        p.appendChild(b);
    }});
    msgsEl.scrollTop = msgsEl.scrollHeight;
}}

function autoResize() {{
    inputEl.style.height = 'auto';
    inputEl.style.height = Math.min(inputEl.scrollHeight, 150) + 'px';
}}

inputEl.addEventListener('input', autoResize);

async function sendMessage() {{
    const text = inputEl.value.trim();
    if (!text) return;
    addMessage(text, 'user');
    inputEl.value = '';
    inputEl.style.height = 'auto';
    statusEl.textContent = '发送中...';
    sendBtn.disabled = true;

    try {{
        const res = await fetch('/send', {{
            method: 'POST',
            headers: {{ 'Content-Type': 'application/json' }},
            body: JSON.stringify({{ message: text }})
        }});
        const data = await res.json();
        if (data.success) {{
            statusEl.textContent = '已发送';
        }} else {{
            statusEl.textContent = '失败: ' + (data.error || 'unknown');
        }}
    }} catch (e) {{
        statusEl.textContent = '连接错误';
    }}
    sendBtn.disabled = false;
}}

sendBtn.addEventListener('click', sendMessage);
inputEl.addEventListener('keydown', (e) => {{
    if (e.key === 'Enter' && !e.shiftKey) {{
        e.preventDefault();
        sendMessage();
    }}
}});

const evtSource = new EventSource('/stream');
evtSource.onmessage = (event) => {{
    addMessage(event.data, 'agent');
    statusEl.textContent = '已连接';
}};
evtSource.onerror = () => {{
    statusEl.textContent = '重连中...';
}};
</script>
</body>
</html>"""


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