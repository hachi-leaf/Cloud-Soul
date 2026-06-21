#!/usr/bin/python3
"""Cloud-Soul 终端对话 - 实时回复 + 保护正在输入的文本"""
import sys, threading, termios, tty, select, rclpy
from datetime import datetime, timezone
from rclpy.node import Node
from std_msgs.msg import String

BOLD   = "\033[1m"; DIM = "\033[2m"
GREEN  = "\033[92m"; CYAN = "\033[96m"
YELLOW = "\033[93m"; GRAY = "\033[90m"
RESET  = "\033[0m"

def ts():
    return datetime.now(timezone.utc).strftime("%H:%M:%S")

class ChatNode(Node):
    def __init__(self, agent_name="adam"):
        super().__init__("chat_node")
        self.agent = agent_name
        self.pub = self.create_publisher(String, f"/{agent_name}/master_chat", 10)
        self.sub = self.create_subscription(String, f"/{agent_name}/response", self.on_response, 10)
        self.pending = []          # 积压回复（有输入时暂存）
        self.input_buf = ""        # 当前正在输入的文本
        self.in_input = False      # 是否正在输入
        self.lock = threading.Lock()
        self.running = True

        print(f"\n{GREEN}{BOLD}╭──────────────────────────────────────────╮{RESET}")
        print(f"{GREEN}{BOLD}│{RESET}   Cloud-Soul Terminal Chat             {GREEN}{BOLD}│{RESET}")
        print(f"{GREEN}{BOLD}│{RESET}   Agent: {CYAN}{agent_name:<31}{GREEN}{BOLD}│{RESET}")
        print(f"{GREEN}{BOLD}│{RESET}   Ctrl+C 退出  /help 查看帮助          {GREEN}{BOLD}│{RESET}")
        print(f"{GREEN}{BOLD}╰──────────────────────────────────────────╯{RESET}\n")

        self.input_thread = threading.Thread(target=self.read_char_input, daemon=True)
        self.input_thread.start()

    def flush_pending(self):
        with self.lock:
            if not self.pending:
                return
            # 上移光标到输入行，清除，打印积压回复，再打印提示符+已输入内容
            if self.in_input:
                sys.stdout.write("\r\033[K")  # 清除当前行
            for content in self.pending:
                sys.stdout.write(f"{CYAN}{BOLD}Adam{RESET} {DIM}[{ts()}]{RESET}\n")
                sys.stdout.write(f"  {content}\n\n")
            sys.stdout.flush()
            self.pending.clear()
            if self.in_input:
                sys.stdout.write(f"{BOLD}{YELLOW}▸ {RESET}{self.input_buf}")
                sys.stdout.flush()

    def read_char_input(self):
        """逐字符读取，支持实时显示 + 回复时不丢已输入内容"""
        fd = sys.stdin.fileno()
        old_settings = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            while self.running:
                # 先检查有无积压回复
                with self.lock:
                    if self.pending:
                        self.flush_pending()

                # 非阻塞读取
                if select.select([sys.stdin], [], [], 0.1)[0]:
                    ch = sys.stdin.read(1)
                    if ch == '\x03':  # Ctrl+C
                        self.running = False
                        break
                    elif ch in ('\r', '\n'):  # Enter
                        self.flush_pending()
                        sys.stdout.write("\r\033[K")
                        line = self.input_buf.strip()
                        self.input_buf = ""
                        self.in_input = False
                        if line == "/help":
                            sys.stdout.write(f"{GREEN}命令:{RESET}\n  直接输入文字  发送给 Agent\n  /help         显示此帮助\n  Ctrl+C        退出\n\n{GREEN}提示:{RESET}\n  Adam 的回复实时显示，正在输入的文本不会被破坏。\n\n")
                        elif line:
                            sys.stdout.write(f"{DIM}{GRAY}  [{ts()}] 已发送{RESET}\n")
                            msg = String(); msg.data = line
                            self.pub.publish(msg)
                        # 重新显示提示符
                        sys.stdout.write(f"{BOLD}{YELLOW}▸ {RESET}")
                        sys.stdout.flush()
                    elif ch == '\x7f':  # Backspace
                        if self.input_buf:
                            self.input_buf = self.input_buf[:-1]
                            sys.stdout.write("\b \b")
                            sys.stdout.flush()
                    elif ord(ch) >= 32:  # 可打印字符
                        self.input_buf += ch
                        self.in_input = True
                        sys.stdout.write(ch)
                        sys.stdout.flush()
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)

    def on_response(self, msg):
        with self.lock:
            self.pending.append(msg.data)
        # 立即触发刷新
        self.flush_pending()

    def destroy_node(self):
        self.running = False
        super().destroy_node()

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
        # 恢复终端
        print(f"\n{GRAY}会话结束。{RESET}")

if __name__ == "__main__":
    main()
