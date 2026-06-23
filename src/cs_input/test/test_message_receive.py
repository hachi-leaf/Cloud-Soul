#!/usr/bin/env python3
# Copyright (c) 2026 Leaf
# SPDX-License-Identifier: MIT
"""
message_receive_node 功能测试脚本

验证：
1. 服务调用成功并返回正确响应
2. 快照中包含带前缀的消息
3. 多条消息累积
4. 特殊字符处理
5. 空消息处理
6. 无人发消息时快照为空
"""

import json
import subprocess
import sys
import time

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
from std_msgs.msg import String
from cs_interfaces.srv import GetSnapshot, SendMessage

AGENT_NAME = "test_agent"
INFO_TIMEOUT = 3.0
DISCOVERY_PERIOD = 1.0

INPUT_MGMT_NODE = "input_mgmt_node"
MESSAGE_RECEIVE_NODE = "message_receive_node"

class MessageReceiveTester(Node):
    def __init__(self):
        super().__init__('message_receive_tester')
        self.data_sub = self.create_subscription(
            String,
            f'/{AGENT_NAME}/input/message_receive',
            self.data_cb,
            QoSProfile(depth=1000, reliability=ReliabilityPolicy.RELIABLE,
                       durability=DurabilityPolicy.TRANSIENT_LOCAL))
        self.data_received = []

        self.snapshot_client = self.create_client(GetSnapshot, f'/{AGENT_NAME}/input')
        while not self.snapshot_client.wait_for_service(timeout_sec=0.1):
            self.get_logger().warn('Waiting for snapshot service...')

        self.send_client = self.create_client(SendMessage, f'/{AGENT_NAME}/input/message_receive/ros2_msg')
        while not self.send_client.wait_for_service(timeout_sec=0.1):
            self.get_logger().warn('Waiting for message_receive service...')

    def data_cb(self, msg):
        self.data_received.append(msg.data)

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

    def send_message(self, text, timeout=2.0):
        req = SendMessage.Request()
        req.message = text
        future = self.send_client.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done() and future.result() is not None:
            return future.result()
        return None

    def clear_history(self):
        self.data_received.clear()


class TestRunner:
    def __init__(self):
        self.mgmt_proc = None
        self.receive_proc = None
        self.tester = None
        self.executor = None

    def setUp(self):
        # 清理残留进程
        subprocess.run(['pkill', '-9', '-f', INPUT_MGMT_NODE], stderr=subprocess.DEVNULL)
        subprocess.run(['pkill', '-9', '-f', MESSAGE_RECEIVE_NODE], stderr=subprocess.DEVNULL)
        time.sleep(0.5)

        # 启动 input_mgmt_node
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

        # 启动 message_receive_node
        self.receive_proc = subprocess.Popen(
            ['ros2', 'run', 'cs_input', MESSAGE_RECEIVE_NODE,
             '--ros-args', '-p', f'agent_name:={AGENT_NAME}',
             '-p', 'info_rate:=2.0'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(2.0)

        if self.receive_proc.poll() is not None:
            out, err = self.receive_proc.communicate()
            print(f"消息接收节点启动失败！\nstdout: {out}\nstderr: {err}")
            sys.exit(1)

        rclpy.init()
        self.tester = MessageReceiveTester()
        self.executor = rclpy.executors.SingleThreadedExecutor()
        self.executor.add_node(self.tester)
        self.spin_for(1.0)
        print("节点已启动，开始测试\n")

    def tearDown(self):
        # 停止 message_receive_node
        self.stop_receive()
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
        subprocess.run(['pkill', '-9', '-f', MESSAGE_RECEIVE_NODE], stderr=subprocess.DEVNULL)

    def spin_for(self, duration):
        deadline = time.time() + duration
        while time.time() < deadline:
            self.executor.spin_once(timeout_sec=0.05)

    def stop_receive(self):
        if self.receive_proc and self.receive_proc.poll() is None:
            self.receive_proc.terminate()
            try:
                self.receive_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.receive_proc.kill()
                self.receive_proc.wait()
            self.receive_proc = None

    # ---------- 测试用例 ----------
    def test1_service_call_success(self):
        print("=== 测试1: 服务调用成功并返回正确响应 ===")
        res = self.tester.send_message("hello")
        assert res is not None, "服务调用失败"
        print(f"  响应: success={res.success}, message='{res.message}'")
        assert res.success == True, "success 应为 True"
        assert res.message == "消息已发送", f"message 内容不符: {res.message}"
        print("✓ 通过\n")

    def test2_snapshot_prefix(self):
        print("=== 测试2: 快照包含带正确前缀的消息 ===")
        self.tester.call_snapshot()
        self.tester.send_message("测试消息")
        self.spin_for(1.0)
        snap = self.tester.call_snapshot()
        print(f"  快照: {json.dumps(snap, ensure_ascii=False)}")
        assert snap is not None and "message_receive" in snap, "快照中未包含 message_receive"
        msgs = snap["message_receive"]
        assert len(msgs) == 1, f"期望1条消息，实际 {len(msgs)}"
        prefix = msgs[0]
        assert prefix.startswith("[20") and "+ros2_msg]" in prefix, f"前缀格式错误: {prefix}"
        assert "测试消息" in prefix, f"消息内容缺失: {prefix}"
        print("✓ 通过\n")

    def test3_multiple_messages(self):
        print("=== 测试3: 多条消息累积 ===")
        self.tester.call_snapshot()
        self.tester.send_message("msg1")
        self.tester.send_message("msg2")
        self.tester.send_message("msg3")
        self.spin_for(1.0)
        snap = self.tester.call_snapshot()
        print(f"  快照: {json.dumps(snap, ensure_ascii=False)}")
        msgs = snap.get("message_receive", [])
        assert len(msgs) == 3, f"期望3条消息，实际 {len(msgs)}"
        assert "msg1" in msgs[0] and "msg2" in msgs[1] and "msg3" in msgs[2]
        print("✓ 通过\n")

    def test4_special_characters(self):
        print("=== 测试4: 特殊字符处理 ===")
        self.tester.call_snapshot()
        special_text = '他说 "你好"\n换行\t制表'
        self.tester.send_message(special_text)
        self.spin_for(1.0)
        snap = self.tester.call_snapshot()
        print(f"  快照: {json.dumps(snap, ensure_ascii=False)}")
        msgs = snap.get("message_receive", [])
        assert len(msgs) == 1
        assert "他说" in msgs[0]
        print("✓ 通过\n")

    def test5_empty_message(self):
        print("=== 测试5: 空消息处理 ===")
        self.tester.call_snapshot()
        self.tester.send_message("")
        self.spin_for(1.0)
        snap = self.tester.call_snapshot()
        print(f"  快照: {json.dumps(snap, ensure_ascii=False)}")
        msgs = snap.get("message_receive", [])
        assert len(msgs) == 1, "空消息也应被发布"
        assert "[20" in msgs[0] and "+ros2_msg]" in msgs[0], "空消息仍有前缀"
        print("✓ 通过\n")

    def test6_no_message_no_appear(self):
        print("=== 测试6: 无人发消息时快照为空 ===")
        self.tester.call_snapshot()
        snap = self.tester.call_snapshot()
        print(f"  快照: {json.dumps(snap, ensure_ascii=False)}")
        assert snap == {}, f"预期空快照，实际: {snap}"
        print("✓ 通过\n")


def main():
    runner = TestRunner()
    try:
        runner.setUp()
        runner.test6_no_message_no_appear()
        runner.test1_service_call_success()
        runner.test2_snapshot_prefix()
        runner.test3_multiple_messages()
        runner.test4_special_characters()
        runner.test5_empty_message()
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