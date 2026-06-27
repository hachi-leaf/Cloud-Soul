#!/usr/bin/env python3
"""
input_mgmt_node 全状态穷举测试（稳定版）
用法: python3 test_input_mgmt.py <agent_name>
"""

import rclpy
from rclpy.node import Node
from std_msgs.msg import String
from cs_interfaces.srv import GetSnapshot
import json, time, sys, subprocess, os, signal

AGENT = sys.argv[1] if len(sys.argv) > 1 else 'test_agent'
MGMT_NS = f'/{AGENT}/input'
PASS = 0; FAIL = 0; SKIP = 0

def check(desc, cond, details=None):
    global PASS, FAIL
    if cond: PASS += 1; print(f"  [OK] {desc}")
    else: FAIL += 1; print(f"  [FAIL] {desc}" + (f" | {details}" if details else ""))

class MockInputPublisher(Node):
    def __init__(self, src_name, mode='accumulate', desc='mock'):
        super().__init__(f'mock_{src_name}')
        self.info_pub = self.create_publisher(String, f'{MGMT_NS}/{src_name}/info', 10)
        self.data_pub = self.create_publisher(String, f'{MGMT_NS}/{src_name}', 10)
        self.mode = mode
        self.desc = desc

        # 等待管理节点订阅 info 话题
        self._wait_for_subscriber(self.info_pub, 'info')
        # 确保 info 被收到（重复发送）
        self.send_info()
        time.sleep(0.5)
        # 等待管理节点订阅 data 话题
        self._wait_for_subscriber(self.data_pub, 'data')
        time.sleep(0.2)

    def _wait_for_subscriber(self, pub, name, timeout=15.0):
        start = time.time()
        while pub.get_subscription_count() == 0:
            if time.time() - start > timeout:
                print(f"  WARNING: {name} subscriber did not appear after {timeout}s")
                break
            time.sleep(0.2)

    def send_info(self):
        msg = String()
        msg.data = json.dumps({'desc': self.desc, 'mode': self.mode, 'status': 'ok'})
        self.info_pub.publish(msg)

    def send_data(self, text):
        msg = String()
        msg.data = text
        self.data_pub.publish(msg)

class Tester(Node):
    def __init__(self):
        super().__init__('tester')
        self.cli = self.create_client(GetSnapshot, MGMT_NS)
        self.cli.wait_for_service(timeout_sec=5.0)

    def get_snapshot(self, timeout=3.0):
        req = GetSnapshot.Request()
        future = self.cli.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done():
            return json.loads(future.result().snapshot_json)
        return None

def start_mgmt():
    proc = subprocess.Popen(
        ['ros2', 'run', 'cs_input', 'input_mgmt_node', '--ros-args',
         '-p', f'agent_name:={AGENT}'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(5)   # 等待节点启动 + 至少一次发现扫描
    return proc

def kill_mgmt(proc):
    os.kill(proc.pid, signal.SIGINT)
    proc.wait(5)

# ------ 初始化 ------
rclpy.init(args=sys.argv)
mgmt_proc = start_mgmt()
tester = Tester()

# ====== T1: accumulate ======
print("\n=== T1: accumulate 模式 ===")
src1 = MockInputPublisher('src1', mode='accumulate')
src1.send_data('msg1')
src1.send_data('msg2')
time.sleep(0.5)
snap = tester.get_snapshot()
check("T1 snapshot contains src1", 'src1' in snap and len(snap['src1']) == 2, f"snap={snap}")
snap2 = tester.get_snapshot()
check("T1 second snapshot empty", snap2 == {}, f"snap2={snap2}")

# ====== T2: latest ======
print("\n=== T2: latest 模式 ===")
src2 = MockInputPublisher('src2', mode='latest')
src2.send_data('first')
src2.send_data('second')
time.sleep(0.5)
snap = tester.get_snapshot()
check("T2 only latest", 'src2' in snap and snap['src2'] == ['second'], f"snap={snap}")

# ====== T3: 默认 accumulate ======
print("\n=== T3: mode 缺失 → accumulate ===")
src3 = MockInputPublisher('src3', mode='')   # 空字符串
src3.send_data('a')
src3.send_data('b')
time.sleep(0.5)
snap = tester.get_snapshot()
check("T3 default accumulate", snap.get('src3') == ['a','b'], f"snap={snap}")

# ====== T4: 超时移除 ======
print("\n=== T4: 超时移除 ===")
src4 = MockInputPublisher('src4', mode='accumulate')
src4.send_data('x')
time.sleep(6.0)          # info_timeout=3s + 余量（期间不发送 info）
snap = tester.get_snapshot()
check("T4 source removed after timeout", 'src4' not in snap, f"snap={snap}")

# ====== T5: 特殊字符 ======
print("\n=== T5: 特殊字符转义 ===")
src5 = MockInputPublisher('src5', mode='accumulate')
src5.send_data('line1\nline2\t"quoted"')
time.sleep(0.5)
snap = tester.get_snapshot()
if 'src5' in snap:
    raw = snap['src5'][0]
    check("T5 escaped data", '\n' in raw and '\t' in raw and '"' in raw, f"raw={repr(raw)}")
else:
    check("T5 no data", False)

# ------ 清理 ------
kill_mgmt(mgmt_proc)
tester.destroy_node()
rclpy.shutdown()

total = PASS + FAIL + SKIP
print(f"\n{'='*60}\n  Pass: {PASS}  Fail: {FAIL}  Skip: {SKIP}\n{'='*60}")
sys.exit(0 if FAIL == 0 else 1)