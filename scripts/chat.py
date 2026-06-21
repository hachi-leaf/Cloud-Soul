#!/usr/bin/python3
import sys, threading, rclpy
from rclpy.node import Node
from std_msgs.msg import String

class ChatNode(Node):
    def __init__(self, agent_name="adam"):
        super().__init__("chat_node")
        self.agent = agent_name
        self.pub = self.create_publisher(String, f"/{agent_name}/master_chat", 10)
        self.sub = self.create_subscription(String, f"/{agent_name}/response", self.on_response, 10)
        self.running = True
        self.input_thread = threading.Thread(target=self.read_stdin, daemon=True)
        self.input_thread.start()

    def read_stdin(self):
        while self.running:
            try:
                line = sys.stdin.readline()
                if not line:
                    break
                line = line.strip()
                if line:
                    msg = String()
                    msg.data = line
                    self.pub.publish(msg)
            except (EOFError, KeyboardInterrupt):
                break
        self.running = False

    def on_response(self, msg):
        print(f"Adam: {msg.data}", flush=True)

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

if __name__ == "__main__":
    main()
