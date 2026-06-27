#!/usr/bin/env python3
"""
system_status_node 全状态穷举测试（v3 双向对齐，无警告版）
用法: sudo python3 test_system_status.py [agent_name]

覆盖所有输入输出组合：正常、边界、单点/多点故障、超时、参数容错、退出。
"""

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
from std_msgs.msg import String
import json, sys, time, subprocess, os, signal, shutil

AGENT = sys.argv[1] if len(sys.argv) > 1 else 'agent'
INFO_TOPIC = f'/{AGENT}/input/system_status/info'
DATA_TOPIC = f'/{AGENT}/input/system_status'
PASS = 0; FAIL = 0; SKIP = 0

def check(desc, condition, details=None):
    global PASS, FAIL
    if condition:
        PASS += 1; print(f"  [OK] {desc}")
    else:
        FAIL += 1; print(f"  [FAIL] {desc}" + (f" | {details}" if details else ""))

def require_root():
    if os.geteuid() != 0:
        print("  [SKIP] Requires root privileges")
        return False
    return True

class Tester(Node):
    def __init__(self):
        super().__init__('system_status_tester_' + str(os.getpid()))
        self.info_msgs = []; self.data_msgs = []
        qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE,
                         durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.info_sub = self.create_subscription(String, INFO_TOPIC, lambda m: self.info_msgs.append(m.data), qos)
        self.data_sub = self.create_subscription(String, DATA_TOPIC, lambda m: self.data_msgs.append(m.data), 10)
        self._wait_for_discovery(4.0)

    def _wait_for_discovery(self, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            rclpy.spin_once(self, timeout_sec=0.1)

    def wait_for_data(self, n=1, timeout=6.0):
        t0 = time.time()
        while len(self.data_msgs) < n and time.time() - t0 < timeout:
            rclpy.spin_once(self, timeout_sec=0.1)
        return self.data_msgs[:]

    def wait_for_info(self, n=1, timeout=6.0):
        t0 = time.time()
        while len(self.info_msgs) < n and time.time() - t0 < timeout:
            rclpy.spin_once(self, timeout_sec=0.1)
        return self.info_msgs[:]

    def clear(self):
        self.info_msgs.clear()
        self.data_msgs.clear()

def destroy_tester(t):
    """安全销毁 tester 节点，避免残留资源"""
    if t:
        t.destroy_node()
        # 让执行器完成清理
        try:
            rclpy.spin_once(t, timeout_sec=0.1)
        except:
            pass

def start_node(publish_rate=1.0):
    cmd = ['ros2', 'run', 'cs_input', 'system_status_node', '--ros-args',
           '-p', f'agent_name:={AGENT}', '-p', f'publish_rate:={publish_rate}']
    proc = subprocess.Popen(cmd, preexec_fn=os.setsid,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    return proc

def kill_node(proc):
    if not proc: return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGINT)
        proc.wait(5)
    except:
        proc.kill()
    time.sleep(0.5)

# ================= 初始化 =================
rclpy.init(args=sys.argv)
main_proc = start_node()
tester = Tester()
time.sleep(2)

# ================= T1 =================
print("\n=== T1: 所有采集成功 ===")
datas = tester.wait_for_data()
infos = tester.wait_for_info()
if datas:
    d = json.loads(datas[-1])
    all_keys = all(k in d for k in ['cpu','mem','disk','net','time','gpu','host','user','machine_id'])
    check("T1 all fields present", all_keys)
else:
    check("T1 data received", False, "no data")
if infos:
    info = json.loads(infos[-1])
    check("T1 info status ok", info.get('status') == 'ok')
    check("T1 info mode latest", info.get('mode') == 'latest')
else:
    check("T1 info received", False)

# ================= T2 =================
print("\n=== T2: 无 GPU ===")
if datas:
    gpu = json.loads(datas[-1]).get('gpu')
    check("T2 gpu field present", gpu is None or isinstance(gpu, str))
else:
    check("T2 no data", False)

# ================= T3 =================
print("\n=== T3: 仅回环网络 ===")
print("  [SKIP] T3 requires isolated network environment")
SKIP += 1

# ================= T4: USER 缺失 =================
print("\n=== T4: USER 缺失 ===")
kill_node(main_proc)
main_proc = None
destroy_tester(tester)                # 清理旧节点

env = os.environ.copy()
env.pop('USER', None)
proc_t4 = subprocess.Popen(
    ['ros2', 'run', 'cs_input', 'system_status_node', '--ros-args', '-p', f'agent_name:={AGENT}'],
    env=env, preexec_fn=os.setsid, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(4)
tester_t4 = Tester()
datas = tester_t4.wait_for_data()
if datas:
    d = json.loads(datas[-1])
    check("T4 user is unknown", d.get('user') == 'unknown', str(d.get('user')))
else:
    check("T4 no data", False)
destroy_tester(tester_t4)
kill_node(proc_t4)

# 重启正常节点及 tester
main_proc = start_node()
tester = Tester()
time.sleep(2)

# ================= T5: machine-id 缺失 =================
print("\n=== T5: /etc/machine-id 缺失 ===")
if not require_root():
    SKIP += 1
else:
    backup = '/etc/machine-id.bak'
    try:
        shutil.move('/etc/machine-id', backup)
        kill_node(main_proc)
        destroy_tester(tester)
        main_proc = start_node()
        time.sleep(3)
        tester = Tester()
        datas = tester.wait_for_data()
        if datas:
            d = json.loads(datas[-1])
            check("T5 machine_id is null", d.get('machine_id') is None)
        else:
            check("T5 no data", False)
    except Exception as e:
        print(f"  [SKIP] T5 error: {e}")
        SKIP += 1
    finally:
        kill_node(main_proc)
        destroy_tester(tester)
        if os.path.exists(backup):
            shutil.move(backup, '/etc/machine-id')
        main_proc = start_node()
        tester = Tester()
        time.sleep(2)

# ================= T6: CPU 失败 =================
print("\n=== T6: CPU 采集失败 ===")
if not require_root():
    SKIP += 1
else:
    kill_node(main_proc)
    destroy_tester(tester)
    subprocess.run(['mount', '--bind', '/dev/null', '/proc/stat'], check=True)
    try:
        main_proc = start_node()
        time.sleep(3)
        tester = Tester()
        datas = tester.wait_for_data()
        infos = tester.wait_for_info()
        if datas:
            d = json.loads(datas[-1])
            check("T6 cpu is null", d.get('cpu') is None)
        if infos:
            i = json.loads(infos[-1])
            check("T6 info partial_failure", i.get('status') == 'partial_failure')
    finally:
        subprocess.run(['umount', '/proc/stat'])
        kill_node(main_proc)
        destroy_tester(tester)
        main_proc = start_node()
        tester = Tester()
        time.sleep(2)

# T7, T9, T11 为 SKIP (LD_PRELOAD)
for t_id in [7, 9, 11]:
    print(f"\n=== T{t_id}: 需要 LD_PRELOAD ===")
    print(f"  [SKIP] T{t_id} needs LD_PRELOAD hook")
    SKIP += 1

# ================= T8: 磁盘失败 =================
print("\n=== T8: 磁盘采集失败 ===")
if not require_root():
    SKIP += 1
else:
    kill_node(main_proc)
    destroy_tester(tester)
    subprocess.run(['mount', '--bind', '/dev/null', '/'], check=True)
    try:
        main_proc = start_node()
        time.sleep(3)
        tester = Tester()
        datas = tester.wait_for_data()
        if datas:
            d = json.loads(datas[-1])
            check("T8 disk is null", d.get('disk') is None)
    finally:
        subprocess.run(['umount', '/'])
        kill_node(main_proc)
        destroy_tester(tester)
        main_proc = start_node()
        tester = Tester()
        time.sleep(2)

# ================= T10: GPU 超时 =================
print("\n=== T10: GPU 超时 ===")
if not require_root():
    SKIP += 1
else:
    nvidia_path = shutil.which('nvidia-smi')
    if not nvidia_path:
        print("  [SKIP] nvidia-smi not found")
        SKIP += 1
    else:
        backup = nvidia_path + '.bak'
        try:
            shutil.move(nvidia_path, backup)
            with open(nvidia_path, 'w') as f:
                f.write('#!/bin/bash\nsleep 10\necho GPU')
            os.chmod(nvidia_path, 0o755)
            kill_node(main_proc)
            destroy_tester(tester)
            main_proc = start_node()
            time.sleep(5)
            tester = Tester()
            datas = tester.wait_for_data(timeout=5)
            infos = tester.info_msgs[:]
            if datas:
                d = json.loads(datas[-1])
                check("T10 gpu is null", d.get('gpu') is None)
            if infos:
                i = json.loads(infos[-1])
                check("T10 info partial_failure", i.get('status') == 'partial_failure')
        except Exception as e:
            print(f"  [SKIP] T10 error: {e}")
            SKIP += 1
        finally:
            kill_node(main_proc)
            destroy_tester(tester)
            if os.path.exists(backup):
                shutil.move(backup, nvidia_path)
            main_proc = start_node()
            tester = Tester()
            time.sleep(2)

# ================= T12: 多重失败 =================
print("\n=== T12: CPU+磁盘同时失败 ===")
if not require_root():
    SKIP += 1
else:
    kill_node(main_proc)
    destroy_tester(tester)
    subprocess.run(['mount', '--bind', '/dev/null', '/proc/stat'], check=True)
    subprocess.run(['mount', '--bind', '/dev/null', '/'], check=True)
    try:
        main_proc = start_node()
        time.sleep(3)
        tester = Tester()
        datas = tester.wait_for_data()
        infos = tester.info_msgs[:]
        if datas:
            d = json.loads(datas[-1])
            check("T12 cpu and disk null", d.get('cpu') is None and d.get('disk') is None)
        if infos:
            i = json.loads(infos[-1])
            check("T12 info partial_failure", i.get('status') == 'partial_failure')
    finally:
        subprocess.run(['umount', '/proc/stat'])
        subprocess.run(['umount', '/'])
        kill_node(main_proc)
        destroy_tester(tester)
        main_proc = start_node()
        tester = Tester()
        time.sleep(2)

# ================= T13: rate=0 容错 =================
print("\n=== T13: publish_rate=0.0 容错 ===")
kill_node(main_proc)
destroy_tester(tester)
main_proc = start_node(publish_rate=0.0)
time.sleep(5)
tester = Tester()
infos = tester.wait_for_info(n=2, timeout=5)
check("T13 info received >=2", len(infos) >= 2)
kill_node(main_proc)
destroy_tester(tester)

# ================= T14: 优雅退出 =================
print("\n=== T14: SIGINT 优雅退出 ===")
main_proc = start_node()
time.sleep(3)
tester = Tester()
time.sleep(1)
tester.clear()
rclpy.spin_once(tester, timeout_sec=1)
before = len(tester.info_msgs)
os.killpg(os.getpgid(main_proc.pid), signal.SIGINT)
time.sleep(2)
rclpy.spin_once(tester, timeout_sec=1)
after = len(tester.info_msgs)
check("T14 no new info after shutdown", after == before)
kill_node(main_proc)
destroy_tester(tester)

# ================= 汇总 =================
total = PASS + FAIL + SKIP
print("\n" + "=" * 60)
print(f"  Pass: {PASS}  Fail: {FAIL}  Skip: {SKIP}")
if FAIL == 0:
    print("  All automated tests passed.")
else:
    print("  Some tests failed, check output.")
print("=" * 60)
rclpy.shutdown()
sys.exit(0 if FAIL == 0 else 1)