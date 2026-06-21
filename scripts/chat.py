#!/usr/bin/python3
"""
Cloud-Soul 终端对话
单线程事件循环：交替处理 ROS2 回调和终端输入，零竞态。
"""
import sys, os, codecs, termios, tty, select, rclpy
from datetime import datetime, timezone
from rclpy.node import Node
from rclpy.executors import SingleThreadedExecutor
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
        self.pending = []            # Agent 回复队列
        self.input_buf = ""          # 当前输入行
        self.running = True
        self.in_input = False        # 是否处于输入模式
        self.decoder = codecs.getincrementaldecoder('utf-8')()

    def on_response(self, msg):
        """ROS2 回调：仅追加到队列，不做 IO"""
        self.pending.append(msg.data)

    def flush_pending(self):
        """在输入行上方打印所有待显示回复，然后恢复提示符"""
        if not self.pending:
            return
        msgs = self.pending
        self.pending = []
        # 始终先清除当前行（无论是否在输入中）
        sys.stdout.write("\r\033[K")
        for content in msgs:
            sys.stdout.write(f"{CYAN}{BOLD}Adam{RESET} {DIM}[{ts()}]{RESET}\r\n")
            for line in content.split('\n'):
                sys.stdout.write(f"  {line}\r\n")
        # 始终恢复提示符（输入中恢复带缓冲区，否则仅箭头）
        if self.in_input:
            sys.stdout.write(f"\r{BOLD}{YELLOW}▸ {RESET}{self.input_buf}")
        else:
            sys.stdout.write(f"{BOLD}{YELLOW}▸ {RESET}")
        sys.stdout.flush()

    def send_line(self, line: str):
        """发送一行文本到 Agent"""
        line = line.strip()
        if not line:
            return
        if line == "/help":
            self.pending.append(
                "命令:\n"
                "  直接输入文字  发送给 Agent\n"
                "  /help         显示此帮助\n"
                "  Ctrl+C        退出\n"
                "\n"
                "提示:\n"
                "  Adam 的回复实时显示，正在输入的文本不会被破坏。"
            )
            return
        sys.stdout.write(f"{DIM}{GRAY}  [{ts()}] 已发送{RESET}\r\n")
        msg = String(); msg.data = line
        self.pub.publish(msg)

def main():
    rclpy.init()
    agent = sys.argv[1] if len(sys.argv) > 1 else "adam"
    node = ChatNode(agent)

    # 打印头部
    print(f"\n{GREEN}{BOLD}╭──────────────────────────────────────────╮{RESET}")
    print(f"{GREEN}{BOLD}│{RESET}   Cloud-Soul Terminal Chat             {GREEN}{BOLD}│{RESET}")
    print(f"{GREEN}{BOLD}│{RESET}   Agent: {CYAN}{agent:<31}{GREEN}{BOLD}│{RESET}")
    print(f"{GREEN}{BOLD}│{RESET}   Ctrl+C 退出  /help 查看帮助          {GREEN}{BOLD}│{RESET}")
    print(f"{GREEN}{BOLD}╰──────────────────────────────────────────╯{RESET}\n")
    sys.stdout.write(f"{BOLD}{YELLOW}▸ {RESET}")
    sys.stdout.flush()

    # 设置终端为 raw 模式
    fd = sys.stdin.fileno()
    old_attrs = termios.tcgetattr(fd)
    tty.setraw(fd)

    # 单线程执行器
    executor = SingleThreadedExecutor()
    executor.add_node(node)

    try:
        while rclpy.ok() and node.running:
            # 1) 处理 ROS2 回调（非阻塞，~20ms 粒度）
            executor.spin_once(timeout_sec=0.02)

            # 2) 显示 Agent 回复
            node.flush_pending()

            # 3) 读取终端输入（非阻塞）
            r, _, _ = select.select([sys.stdin], [], [], 0.02)
            if sys.stdin not in r:
                continue

            data = os.read(fd, 64)
            if not data:
                continue

            text = node.decoder.decode(data, final=False)
            if not text:
                continue

            for ch in text:
                if ch == '\x03':  # Ctrl+C
                    node.running = False
                    break
                elif ch in ('\r', '\n'):  # Enter
                    line = node.input_buf
                    node.input_buf = ""
                    node.in_input = False
                    sys.stdout.write("\r\n")
                    node.send_line(line)
                    node.flush_pending()
                    sys.stdout.write(f"{BOLD}{YELLOW}▸ {RESET}")
                    sys.stdout.flush()
                elif ch == '\x7f':  # Backspace
                    if node.input_buf:
                        node.input_buf = node.input_buf[:-1]
                        sys.stdout.write("\b \b")
                        sys.stdout.flush()
                elif ord(ch) >= 32:  # 可打印字符
                    node.input_buf += ch
                    node.in_input = True
                    sys.stdout.write(ch)
                    sys.stdout.flush()

    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_attrs)
        node.running = False
        node.destroy_node()
        executor.shutdown()
        rclpy.shutdown()
        print(f"\n{GRAY}会话结束。{RESET}")

if __name__ == "__main__":
    main()
