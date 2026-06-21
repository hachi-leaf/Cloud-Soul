#!/usr/bin/python3
"""Cloud-Soul 终端对话客户端 - 回复积压不打断输入"""
import sys, threading, rclpy
from datetime import datetime, timezone
from rclpy.node import Node
from std_msgs.msg import String

# ── ANSI 颜色 ──────────────────────────────────
BOLD   = "\033[1m"
DIM    = "\033[2m"
GREEN  = "\033[92m"
CYAN   = "\033[96m"
YELLOW = "\033[93m"
GRAY   = "\033[90m"
RESET  = "\033[0m"

def ts():
    return datetime.now(timezone.utc).strftime("%H:%M:%S")

class ChatNode(Node):
    def __init__(self, agent_name="adam"):
        super().__init__("chat_node")
        self.agent = agent_name
        self.pub = self.create_publisher(String, f"/{agent_name}/master_chat", 10)
        self.sub = self.create_subscription(String, f"/{agent_name}/response", self.on_response, 10)
        self.pending = []          # 积压的回复
        self.lock = threading.Lock()
        self.running = True

        print(f"\n{GREEN}{BOLD}╭──────────────────────────────────────────╮{RESET}")
        print(f"{GREEN}{BOLD}│{RESET}   Cloud-Soul Terminal Chat             {GREEN}{BOLD}│{RESET}")
        print(f"{GREEN}{BOLD}│{RESET}   Agent: {CYAN}{agent_name:<31}{GREEN}{BOLD}│{RESET}")
        print(f"{GREEN}{BOLD}│{RESET}   Ctrl+C 退出  /help 查看帮助          {GREEN}{BOLD}│{RESET}")
        print(f"{GREEN}{BOLD}╰──────────────────────────────────────────╯{RESET}")
        print()

        self.input_thread = threading.Thread(target=self.read_stdin, daemon=True)
        self.input_thread.start()

    def flush_pending(self):
        """打印所有积压回复（在用户发送消息后调用）"""
        with self.lock:
            if not self.pending:
                return
            for content in self.pending:
                print(f"\n{CYAN}{BOLD}Adam{RESET} {DIM}[{ts()}]{RESET}")
                print(f"  {content}")
                print()
            self.pending.clear()

    def read_stdin(self):
        while self.running:
            try:
                print(f"{BOLD}{YELLOW}▸ {RESET}", end="", flush=True)
                line = sys.stdin.readline()
                if not line:
                    break
                line = line.strip()
                if not line:
                    self.flush_pending()  # 空行也刷积压
                    continue
                if line == "/help":
                    self.show_help()
                    continue
                msg = String()
                msg.data = line
                self.pub.publish(msg)
                print(f"{DIM}{GRAY}  [{ts()}] 已发送{RESET}")
                self.flush_pending()  # 发完消息后立即显示积压回复
            except (EOFError, KeyboardInterrupt):
                break
        self.running = False

    def on_response(self, msg):
        """收到回复不打印，存入积压队列"""
        with self.lock:
            self.pending.append(msg.data)

    def show_help(self):
        print(f"""
{GREEN}命令:{RESET}
  直接输入文字  发送给 Agent
  /help         显示此帮助
  Ctrl+C        退出
  空行          显示积压的回复

{GREEN}提示:{RESET}
  Adam 的回复在你发送消息后才显示，不会打断你正在输入的内容。
""")

def main():
    rclpy.init()
    agent = sys.argv[1] if len(sys.argv) > 1 else "adam"
    node = ChatNode(agent)
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.running = False
        node.destroy_node()
        rclpy.shutdown()
        print(f"\n{GRAY}会话结束。{RESET}")

if __name__ == "__main__":
    main()
