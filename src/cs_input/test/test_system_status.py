#!/usr/bin/env python3
# Copyright (c) leaf
# SPDX-License-Identifier: MIT
"""
system_status_node 功能测试脚本（简化版）

验证：
1. info 心跳正常发布
2. data 话题周期性发布（状态变化时）
3. 管理节点快照中包含 system_status 数据
4. 节点重启后仍能被管理节点发现并聚合数据
"""

import json
import subprocess
import sys
import time
import os

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
from std_msgs.msg import String
from cs_interfaces.srv import GetSnapshot

AGENT_NAME = "test_agent"
INFO_TIMEOUT = 3.0
DISCOVERY_PERIOD = 1.0

INPUT_MGMT_NODE = "input_mgmt_node"
SYSTEM_STATUS_NODE = "system_status_node"

class SystemStatusTester(Node):
    def __init__(self):
        super().__init__('system_status_tester')
        self.info_sub = self.create_subscription(
            String, f'/{AGENT_NAME}/input/system_status/info',
            self.info_cb,
            QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE,
                       durability=DurabilityPolicy.TRANSIENT_LOCAL))
        self.data_sub = self.create_subscription(
            String, f'/{AGENT_NAME}/input/system_status',
            self.data_cb,
            QoSProfile(depth=1000, reliability=ReliabilityPolicy.RELIABLE,
                       durability=DurabilityPolicy.TRANSIENT_LOCAL))
        self.info_received = []
        self.data_received = []

        self.snapshot_client = self.create_client(GetSnapshot, f'/{AGENT_NAME}/input')
        while not self.snapshot_client.wait_for_service(timeout_sec=0.1):
            self.get_logger().warn('Waiting for snapshot service...')

    def info_cb(self, msg): self.info_received.append(msg.data)
    def data_cb(self, msg): self.data_received.append(msg.data)

    def call_snapshot(self, timeout=2.0):
        req = GetSnapshot.Request()
        future = self.snapshot_client.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done() and future.result() is not None:
            try:
                return json.loads(future.result().snapshot_json)
            except json.JSONDecodeError:
                return None
        return None

    def clear_history(self):
        self.info_received.clear()
        self.data_received.clear()


class TestRunner:
    def __init__(self):
        self.mgmt_proc = None
        self.status_proc = None
        self.tester = None
        self.executor = None

    def setUp(self):
        # 清理残留进程
        subprocess.run(['pkill', '-9', '-f', INPUT_MGMT_NODE], stderr=subprocess.DEVNULL)
        subprocess.run(['pkill', '-9', '-f', SYSTEM_STATUS_NODE], stderr=subprocess.DEVNULL)
        time.sleep(0.5)

        self.mgmt_proc = subprocess.Popen(
            ['ros2', 'run', 'cs_input', INPUT_MGMT_NODE,
             '--ros-args', '-p', f'agent_name:={AGENT_NAME}',
             '-p', f'info_timeout:={INFO_TIMEOUT}',
             '-p', f'discovery_period:={DISCOVERY_PERIOD}'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(2.0)

        if self.mgmt_proc.poll() is not None:
            out, err = self.mgmt_proc.communicate()
            print(f"管理节点启动失败！\nstdout: {out}\nstderr: {err}")
            sys.exit(1)

        rclpy.init()
        self.tester = SystemStatusTester()
        self.executor = rclpy.executors.SingleThreadedExecutor()
        self.executor.add_node(self.tester)
        self.spin_for(1.0)
        print("管理节点已启动，开始测试\n")

    def tearDown(self):
        # 停止 system_status_node
        self.stop_system_status()
        # 停止 input_mgmt_node
        if self.mgmt_proc and self.mgmt_proc.poll() is None:
            self.mgmt_proc.terminate()
            try:
                self.mgmt_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.mgmt_proc.kill()
                self.mgmt_proc.wait()
        # 关闭 rclpy
        if self.tester:
            self.executor.remove_node(self.tester)
            self.tester.destroy_node()
        rclpy.shutdown()
        # 再次确认清理残留（以防万一）
        subprocess.run(['pkill', '-9', '-f', INPUT_MGMT_NODE], stderr=subprocess.DEVNULL)
        subprocess.run(['pkill', '-9', '-f', SYSTEM_STATUS_NODE], stderr=subprocess.DEVNULL)

    def spin_for(self, duration):
        deadline = time.time() + duration
        while time.time() < deadline:
            self.executor.spin_once(timeout_sec=0.05)

    def start_system_status(self, rate=1.0):
        self.status_proc = subprocess.Popen(
            ['ros2', 'run', 'cs_input', SYSTEM_STATUS_NODE,
             '--ros-args', '-p', f'agent_name:={AGENT_NAME}',
             '-p', f'publish_rate:={rate}'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(1.5)

    def stop_system_status(self):
        if self.status_proc and self.status_proc.poll() is None:
            self.status_proc.terminate()
            try:
                self.status_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.status_proc.kill()
                self.status_proc.wait()
            self.status_proc = None

    # ---------- 测试 ----------
    def test1_info_heartbeat(self):
        print("=== 测试1: info 心跳发布 ===")
        self.tester.clear_history()
        self.start_system_status(rate=2.0)
        self.spin_for(3.0)
        cnt = len(self.tester.info_received)
        print(f"收到 {cnt} 条 info 消息")
        assert cnt >= 2, f"info 心跳不足，仅 {cnt} 条"
        print("✓ 通过\n")

    def test2_data_publishing(self):
        print("=== 测试2: data 话题发布 ===")
        self.tester.clear_history()
        self.spin_for(5.0)
        cnt = len(self.tester.data_received)
        print(f"收到 {cnt} 条 data 消息")
        assert cnt >= 1, "未收到任何 data 消息"
        print(f"样本: {self.tester.data_received[0][:100]}...")
        print("✓ 通过\n")

    def test3_snapshot_inclusion(self):
        print("=== 测试3: 快照包含 system_status ===")
        self.tester.call_snapshot()  # 清空之前的累积
        self.spin_for(3.0)
        snap = self.tester.call_snapshot()
        print(f"快照内容: {json.dumps(snap, indent=2, ensure_ascii=False)}")
        assert snap is not None, "快照调用失败"
        assert "system_status" in snap, "快照中未包含 system_status"
        data = snap["system_status"]
        assert isinstance(data, list) and len(data) > 0, "数据列表为空"
        print(f"收集到 {len(data)} 条状态记录")
        print("✓ 通过\n")

    def test4_restart_recovery(self):
        print("=== 测试4: 节点重启后仍能被管理节点发现 ===")
        self.stop_system_status()
        print("已停止 system_status_node")
        self.start_system_status(rate=2.0)
        self.spin_for(3.0)
        snap = self.tester.call_snapshot()
        print(f"重启后快照: {json.dumps(snap, indent=2, ensure_ascii=False)}")
        assert snap and "system_status" in snap, "重启后 system_status 未在快照中出现"
        assert len(snap["system_status"]) > 0, "重启后未收到新数据"
        print("✓ 通过\n")


def main():
    runner = TestRunner()
    try:
        runner.setUp()
        runner.test1_info_heartbeat()
        runner.test2_data_publishing()
        runner.test3_snapshot_inclusion()
        runner.test4_restart_recovery()
        print("="*40)
        print("所有测试通过！")
        print("="*40)
    except Exception as e:
        print("\n测试失败:", e)
        import traceback
        traceback.print_exc()
    finally:
        runner.tearDown()


if __name__ == '__main__':
    main()