#!/usr/bin/python3
"""
Cloud-Soul Web Chat Server
纯 stdlib：SSE 推送 + HTTP POST，零外部依赖。
主线程负责 ROS2 spin（发布/订阅都通过 spin 驱动），
HTTP 线程通过队列与主线程通信。
"""
import http.server
import json
import sys
import os
import threading
import queue
import time
import rclpy
from rclpy.node import Node
from rclpy.executors import SingleThreadedExecutor
from std_msgs.msg import String
from datetime import datetime, timezone

AGENT_NAME = sys.argv[1] if len(sys.argv) > 1 else "adam"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

# ============================================
# 全局消息队列
# ============================================
sse_clients = []             # [(queue.Queue, lock), ...]
sse_clients_lock = threading.Lock()
outgoing_queue = queue.Queue()  # 待发送到 Agent 的消息

# ============================================
# ROS2 Node
# ============================================
class BridgeNode(Node):
    def __init__(self):
        super().__init__("webchat_bridge")
        self.pub = self.create_publisher(String, f"/{AGENT_NAME}/master_chat", 10)
        self.sub = self.create_subscription(String, f"/{AGENT_NAME}/response", self.on_response, 10)

    def on_response(self, msg):
        """ROS2 回调：广播到所有 SSE 客户端"""
        payload = json.dumps({
            "from": "adam",
            "text": msg.data,
            "ts": datetime.now(timezone.utc).strftime("%H:%M:%S")
        })
        with sse_clients_lock:
            dead = []
            for q, _ in sse_clients:
                try:
                    q.put_nowait(payload)
                except queue.Full:
                    dead.append((q, _))
            for d in dead:
                try:
                    sse_clients.remove(d)
                except ValueError:
                    pass

    def publish_message(self, text: str):
        msg = String()
        msg.data = text
        self.pub.publish(msg)

# ============================================
# HTML 前端（内嵌）
# ============================================
HTML = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Cloud-Soul Chat - """ + AGENT_NAME + r"""</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{
  font-family:'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif;
  background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);
  height:100vh;display:flex;justify-content:center;align-items:center;
  color:#e0e0e0;
}
#app{
  width:100%;max-width:800px;height:95vh;
  background:rgba(20,20,40,0.85);
  border-radius:16px;border:1px solid rgba(255,255,255,0.08);
  display:flex;flex-direction:column;
  backdrop-filter:blur(20px);
  box-shadow:0 20px 60px rgba(0,0,0,0.5);
}
#header{
  padding:14px 20px;
  border-bottom:1px solid rgba(255,255,255,0.06);
  display:flex;align-items:center;gap:12px;
  flex-shrink:0;
}
#header .dot{width:10px;height:10px;border-radius:50%;background:#4f9}
#header .dot.idle{background:#ff0}
#header h1{font-size:16px;font-weight:600;color:#fff}
#header .sub{font-size:12px;color:#888;margin-left:auto}
#msgs{
  flex:1;overflow-y:auto;padding:20px;
  display:flex;flex-direction:column;gap:12px;
  scroll-behavior:smooth;
}
.msg{max-width:80%;padding:10px 16px;border-radius:14px;line-height:1.5;font-size:14px;word-break:break-word}
.msg .ts{font-size:10px;opacity:0.5;margin-bottom:2px}
.msg.user{align-self:flex-end;background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;border-bottom-right-radius:4px}
.msg.adam{align-self:flex-start;background:rgba(255,255,255,0.06);color:#c8e6c9;border-bottom-left-radius:4px;border:1px solid rgba(255,255,255,0.05)}
.msg.adam pre{background:rgba(0,0,0,0.3);padding:8px 12px;border-radius:8px;overflow-x:auto;font-size:12px;margin:6px 0;border:1px solid rgba(255,255,255,0.06)}
.msg.adam code{font-family:'JetBrains Mono','Fira Code',monospace;font-size:12px}
#input-area{
  padding:12px 20px;border-top:1px solid rgba(255,255,255,0.06);
  display:flex;gap:10px;flex-shrink:0;
}
#input-area input{
  flex:1;padding:10px 16px;border-radius:20px;border:1px solid rgba(255,255,255,0.1);
  background:rgba(255,255,255,0.05);color:#fff;font-size:14px;outline:none;
  transition:border-color 0.2s;
}
#input-area input:focus{border-color:#667eea}
#input-area button{
  padding:10px 20px;border-radius:20px;border:none;
  background:linear-gradient(135deg,#667eea,#764ba2);
  color:#fff;font-size:14px;cursor:pointer;font-weight:600;
  transition:opacity 0.2s;
}
#input-area button:hover{opacity:0.85}
#status{font-size:11px;color:#666;text-align:center;padding:4px;flex-shrink:0}
::-webkit-scrollbar{width:6px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:rgba(255,255,255,0.1);border-radius:3px}
</style>
</head>
<body>
<div id="app">
  <div id="header">
    <div class="dot" id="dot"></div>
    <h1>Cloud-Soul</h1>
    <span style="color:#888;font-size:14px">/ """ + AGENT_NAME + r"""</span>
    <span class="sub" id="conn-status">连接中...</span>
  </div>
  <div id="msgs"></div>
  <div id="status">就绪</div>
  <div id="input-area">
    <input id="inp" type="text" placeholder="输入消息..." autofocus autocomplete="off">
    <button onclick="send()">发送</button>
  </div>
</div>
<script>
const msgs=document.getElementById('msgs');
const inp=document.getElementById('inp');
const dot=document.getElementById('dot');
const connStatus=document.getElementById('conn-status');
const statusEl=document.getElementById('status');

let sse=null;

function scrollBottom(){
  msgs.scrollTop=msgs.scrollHeight;
}

function addMsg(from,text,ts){
  const div=document.createElement('div');
  div.className='msg '+from;
  div.innerHTML='<div class="ts">'+ts+'</div>'+escapeHtml(text);
  msgs.appendChild(div);
  scrollBottom();
}

function escapeHtml(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
    .replace(/```(\w*)\n?([\s\S]*?)```/g,function(_,lang,code){
      return '<pre><code>'+code.replace(/</g,'&lt;').replace(/>/g,'&gt;')+'</code></pre>';
    })
    .replace(/`([^`]+)`/g,'<code>$1</code>')
    .replace(/\n/g,'<br>');
}

function send(){
  const text=inp.value.trim();
  if(!text)return;
  addMsg('user',text,new Date().toTimeString().slice(0,8));
  fetch('/send',{method:'POST',body:text})
    .then(r=>r.text())
    .then(t=>{if(t!=='ok')statusEl.textContent=t})
    .catch(e=>statusEl.textContent='发送失败: '+e);
  inp.value='';
  inp.focus();
}

function connectSSE(){
  if(sse)sse.close();
  sse=new EventSource('/stream');
  sse.onopen=function(){
    dot.className='dot';
    connStatus.textContent='已连接';
    statusEl.textContent='就绪';
  };
  sse.onmessage=function(e){
    try{
      const d=JSON.parse(e.data);
      addMsg(d.from,d.text,d.ts);
      statusEl.textContent='收到回复 @ '+d.ts;
    }catch(err){}
  };
  sse.onerror=function(){
    dot.className='dot idle';
    connStatus.textContent='重连中...';
    statusEl.textContent='连接断开，3秒后重连...';
    sse.close();
    setTimeout(connectSSE,3000);
  };
}

inp.addEventListener('keydown',function(e){
  if(e.key==='Enter')send();
});

connectSSE();
</script>
</body>
</html>"""

# ============================================
# HTTP Request Handler
# ============================================
class ChatHandler(http.server.BaseHTTPRequestHandler):

    def log_message(self, format, *args):
        pass  # 静默日志

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(HTML.encode("utf-8"))
        elif self.path == "/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            client_q = queue.Queue(maxsize=100)
            with sse_clients_lock:
                sse_clients.append((client_q, threading.Lock()))

            try:
                self.wfile.write(f"data: {json.dumps({'from':'system','text':'已连接到 Adam','ts':''})}\n\n".encode())
                self.wfile.flush()
                while True:
                    try:
                        msg = client_q.get(timeout=15)
                        self.wfile.write(f"data: {msg}\n\n".encode())
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(": heartbeat\n\n".encode())
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                with sse_clients_lock:
                    sse_clients[:] = [(q, l) for q, l in sse_clients if q is not client_q]
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path == "/send":
            length = int(self.headers.get("Content-Length", 0))
            text = self.rfile.read(length).decode("utf-8").strip()
            if text:
                outgoing_queue.put(text)  # 放入队列，主线程负责发布
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"ok")
        else:
            self.send_response(404)
            self.end_headers()

# ============================================
# 主函数
# ============================================
def main():
    rclpy.init()
    bridge = BridgeNode()

    executor = SingleThreadedExecutor()
    executor.add_node(bridge)

    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), ChatHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    print(f"\n  Cloud-Soul Web Chat")
    print(f"  Agent: {AGENT_NAME}")
    print(f"  URL:   http://localhost:{PORT}")
    print(f"  按 Ctrl+C 退出\n")

    try:
        while rclpy.ok():
            # 1) 处理待发送消息（主线程发布 + spin，确保消息送达）
            try:
                text = outgoing_queue.get_nowait()
                bridge.publish_message(text)
                # 发布后立即 spin 确保消息发出
                executor.spin_once(timeout_sec=0.1)
            except queue.Empty:
                pass

            # 2) 常规 spin 处理订阅回调
            executor.spin_once(timeout_sec=0.05)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        bridge.destroy_node()
        executor.shutdown()
        rclpy.shutdown()
        print("\n会话结束。")

if __name__ == "__main__":
    main()
