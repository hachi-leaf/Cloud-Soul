#!/usr/bin/env python3
"""
message_receive_node 全状态穷举测试
用法: python3 test_message_receive.py

本脚本自动启动主测试节点 (agent_main) 和多个临时节点，
覆盖 13 项自动化测试。

================================================================
                       穷举测试表 (13 项)
================================================================
S 正常消息 (5):
  T1   ros2_msg 渠道，普通文本
  T2   ros2_msg 渠道，空消息
  T3   web_chat 渠道，正常消息
  T4   特殊字符（换行/引号/制表符）
  T13  复杂 JSON+HTML 嵌套消息（原样传输验证）

I info 心跳 (1):
  T5   info 话题按 info_rate 持续发布，JSON 格式正确

C 参数合法 (2):
  T6   自定义 ros_channel = "custom"
  T7   合法下划线 ros_channel = "my_channel_01"

P 参数非法 (3):
  T8   空 ros_channel ""
  T9   含斜杠 "bad/name"
  T10  含连字符 "bad-channel"  (ROS 2 名称非法)

R info_rate 容错 (2):
  T11  info_rate = 0.0  -> 自动使用 1.0 Hz
  T12  info_rate = -1.0 -> 自动使用 1.0 Hz

跳过: 无
================================================================
"""

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
from std_msgs.msg import String
from cs_interfaces.srv import SendMessage
import json
import sys
import time
import subprocess
import os
import signal

MAIN_AGENT   = "agent_main"
MAIN_CHANNEL = "ros2_msg"
MAIN_NS      = "/" + MAIN_AGENT + "/input/message_receive"
INFO_TOPIC   = MAIN_NS + "/info"
DATA_TOPIC   = MAIN_NS
SRV_ROS      = MAIN_NS + "/" + MAIN_CHANNEL
SRV_WEB      = MAIN_NS + "/web_chat"

PASS = 0
FAIL = 0
SKIP = 0

def check(desc, condition, details=None):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  [OK] {desc}")
    else:
        FAIL += 1
        extra = f" | {details}" if details else ""
        print(f"  [FAIL] {desc}{extra}")

def kill_proc(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGINT)
        proc.wait(5)
    except:
        proc.kill()
    time.sleep(0.5)

def start_node(agent, ros_channel, info_rate=1.0, node_name=None):
    cmd = [
        "ros2", "run", "cs_input", "message_receive_node", "--ros-args",
        "-p", f"agent_name:={agent}",
        "-p", f"ros_channel:={ros_channel}",
        "-p", f"info_rate:={info_rate}"
    ]
    if node_name:
        cmd += ["-r", f"__node:={node_name}"]
    proc = subprocess.Popen(cmd, preexec_fn=os.setsid,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    return proc

def check_node_exited(proc, timeout=2.0):
    try:
        proc.wait(timeout=timeout)
        return proc.returncode
    except subprocess.TimeoutExpired:
        return None

class Tester(Node):
    def __init__(self):
        super().__init__('tester_main_' + str(os.getpid()))
        self.data_msgs = []
        self.info_msgs = []

        self.data_sub = self.create_subscription(
            String, DATA_TOPIC, lambda m: self.data_msgs.append(m.data), 10)

        qos_trans = QoSProfile(
            depth=10,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.info_sub = self.create_subscription(
            String, INFO_TOPIC, lambda m: self.info_msgs.append(m.data), qos_trans)

    def call_service(self, srv_name, message_text, timeout=5.0):
        client = self.create_client(SendMessage, srv_name)
        if not client.wait_for_service(timeout):
            return None
        req = SendMessage.Request()
        req.message = message_text
        future = client.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done():
            return future.result()
        return None

    def clear(self):
        self.data_msgs.clear()
        self.info_msgs.clear()

# ========== 启动主节点 ==========
print("Starting main test node...")
main_proc = start_node(MAIN_AGENT, MAIN_CHANNEL, node_name="main_node")
rclpy.init(args=sys.argv)
tester = Tester()
time.sleep(2)

# ========== S 正常消息 ==========
print("\n=== S 正常消息测试 ===")

# T1
tester.clear()
res = tester.call_service(SRV_ROS, "hello")
time.sleep(0.3)
msgs = tester.data_msgs[:]
check("T1 ros2_msg success", res is not None and res.success)
if msgs:
    check("T1 data prefix ros2_msg", "+ros2_msg" in msgs[-1])
else:
    check("T1 data received", False)

# T2
tester.clear()
res = tester.call_service(SRV_ROS, "")
time.sleep(0.3)
msgs = tester.data_msgs[:]
check("T2 empty message success", res is not None and res.success)
if msgs:
    check("T2 data contains prefix", "+ros2_msg" in msgs[-1])
else:
    check("T2 data received", False)

# T3
tester.clear()
res = tester.call_service(SRV_WEB, "hi from web")
time.sleep(0.3)
msgs = tester.data_msgs[:]
check("T3 web_chat success", res is not None and res.success)
if msgs:
    check("T3 data prefix web_chat", "+web_chat" in msgs[-1])
else:
    check("T3 data received", False)

# T4
tester.clear()
special = "line1\nline2\t\"quoted\""
res = tester.call_service(SRV_ROS, special)
time.sleep(0.3)
msgs = tester.data_msgs[:]
check("T4 special chars success", res is not None and res.success)
if msgs:
    check("T4 contains special",
          "line1" in msgs[-1] and "line2" in msgs[-1] and "\"quoted\"" in msgs[-1])
else:
    check("T4 data received", False)

# T13 : 复杂 JSON+HTML 嵌套消息
tester.clear()
# 构造包含 JSON 内部嵌入 HTML、HTML 属性含 JSON 的复杂字符串
complex_msg = '''
{
  "title": "Test",
  "html": "<div class=\\"main\\" data-json=\\'{\\"key\\":123}\\'><p>Hello</p></div>",
  "nested": {"array": [1, "<script>alert('xss')</script>", null]}
}
'''
res = tester.call_service(SRV_ROS, complex_msg)
time.sleep(0.5)
msgs = tester.data_msgs[:]
check("T13 complex JSON/HTML success", res is not None and res.success)
if msgs:
    last_data = msgs[-1]
    # 验证关键片段存在，确保内容未被破坏
    ok = (
        '"title": "Test"' in last_data and
        '"html": "<div class' in last_data and
        'alert(\'xss\')' in last_data
    )
    check("T13 complex content integrity", ok, f"data[:200]={last_data[:200]}")
else:
    check("T13 data received", False)

# ========== I info 心跳 ==========
print("\n=== I info 心跳测试 ===")
start = time.time()
while time.time() - start < 4.0:
    rclpy.spin_once(tester, timeout_sec=0.1)
infos = tester.info_msgs[:]
check("T5 info received >=2", len(infos) >= 2, f"got {len(infos)}")
if infos:
    try:
        obj = json.loads(infos[-1])
        check("T5 info has desc and mode", "desc" in obj and obj.get("mode") == "accumulate")
    except Exception:
        check("T5 info JSON valid", False)

tester.destroy_node()
rclpy.shutdown()
kill_proc(main_proc)
time.sleep(1)

# ========== C 参数测试 ==========
print("\n=== C 参数测试（合法/非法） ===")

# T6: 自定义合法 channel
print("  -- T6 自定义 ros_channel --")
proc = start_node("agent_t6", "custom", node_name="t6")
res = subprocess.run(
    ["ros2", "service", "call", "/agent_t6/input/message_receive/custom",
     "cs_interfaces/srv/SendMessage", "{message: \"hello\"}"],
    capture_output=True, text=True)
check("T6 custom channel service success", "success=True" in res.stdout)
res2 = subprocess.run(
    ["ros2", "topic", "echo", "--once", "/agent_t6/input/message_receive"],
    capture_output=True, text=True)
check("T6 data contains custom prefix", "+custom" in res2.stdout)
kill_proc(proc)

# T7: 合法下划线 channel
print("  -- T7 合法下划线 ros_channel --")
proc = start_node("agent_t7", "my_channel_01", node_name="t7")
res = subprocess.run(
    ["ros2", "service", "call", "/agent_t7/input/message_receive/my_channel_01",
     "cs_interfaces/srv/SendMessage", "{message: \"ok\"}"],
    capture_output=True, text=True)
check("T7 underscore channel success", "success=True" in res.stdout)
kill_proc(proc)

# T8: 空 channel
print("  -- T8 空 ros_channel --")
proc = start_node("agent_t8", "", node_name="t8")
ret = check_node_exited(proc, timeout=3)
check("T8 node exited on empty channel", ret is not None and ret != 0, f"returncode={ret}")
kill_proc(proc)

# T9: 含斜杠
print("  -- T9 含斜杠 ros_channel --")
proc = start_node("agent_t9", "bad/name", node_name="t9")
ret = check_node_exited(proc, timeout=3)
check("T9 node exited on slash", ret is not None and ret != 0, f"returncode={ret}")
kill_proc(proc)

# T10: 含连字符（ROS 2 非法）
print("  -- T10 含连字符 ros_channel --")
proc = start_node("agent_t10", "bad-channel", node_name="t10")
ret = check_node_exited(proc, timeout=3)
check("T10 node exited on hyphen", ret is not None and ret != 0, f"returncode={ret}")
kill_proc(proc)

# ========== R info_rate 容错 ==========
print("\n=== R info_rate 容错测试 ===")

print("  -- T11 info_rate=0.0 --")
proc = start_node("agent_t11", "ok_ch", info_rate=0.0, node_name="t11")
time.sleep(4)
res = subprocess.run(
    ["ros2", "topic", "echo", "--once",
     "--qos-reliability", "reliable",
     "--qos-durability", "transient_local",
     "/agent_t11/input/message_receive/info"],
    capture_output=True, text=True, timeout=5)
check("T11 info published with rate=0.0", "desc" in res.stdout)
kill_proc(proc)

print("  -- T12 info_rate=-1.0 --")
proc = start_node("agent_t12", "ok_ch", info_rate=-1.0, node_name="t12")
time.sleep(4)
res = subprocess.run(
    ["ros2", "topic", "echo", "--once",
     "--qos-reliability", "reliable",
     "--qos-durability", "transient_local",
     "/agent_t12/input/message_receive/info"],
    capture_output=True, text=True, timeout=5)
check("T12 info published with rate=-1.0", "desc" in res.stdout)
kill_proc(proc)

# ========== 汇总 ==========
total = PASS + FAIL + SKIP
print("\n" + "="*60)
print(f"  Pass: {PASS}  Fail: {FAIL}  Skip: {SKIP}")
if FAIL == 0:
    print("  All automated tests passed.")
else:
    print("  Some tests failed, check output.")
print("="*60)
sys.exit(0 if FAIL == 0 else 1)