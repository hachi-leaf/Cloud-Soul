#!/usr/bin/env python3
# Copyright (c) leaf
# SPDX-License-Identifier: MIT
"""
input_mgmt_node 全量功能测试脚本 (打印真实数据版)
"""

import json
import subprocess
import sys
import time
import threading

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
from std_msgs.msg import String
from cs_interfaces.srv import GetSnapshot

AGENT_NAME = "test_agent"
INFO_TIMEOUT = 3.0
DISCOVERY_PERIOD = 1.0
SETTLE_TIME = 2.5   # 启动后等待 DDS + 发现

class InputTester(Node):
    def __init__(self):
        super().__init__('input_tester', namespace='')
        self.snapshot_client = self.create_client(GetSnapshot, f'/{AGENT_NAME}/input')
        self.pubs = {}
        self.keep_heartbeat = {}

    def wait_for_service(self, timeout=10.0):
        self.get_logger().info('Waiting for snapshot service...')
        if not self.snapshot_client.wait_for_service(timeout):
            self.get_logger().fatal('Service not available!')
            sys.exit(1)

    def create_input_source(self, src_name, info_text="test source", start_heartbeat=True):
        # 使用较大的 depth 避免消息被覆盖
        info_qos = QoSProfile(depth=1000, reliability=ReliabilityPolicy.RELIABLE,
                              durability=DurabilityPolicy.TRANSIENT_LOCAL)
        data_qos = QoSProfile(depth=1000, reliability=ReliabilityPolicy.RELIABLE,
                              durability=DurabilityPolicy.TRANSIENT_LOCAL)
        info_pub = self.create_publisher(String, f'/{AGENT_NAME}/input/{src_name}/info', qos_profile=info_qos)
        data_pub = self.create_publisher(String, f'/{AGENT_NAME}/input/{src_name}', qos_profile=data_qos)
        self.pubs[src_name] = (info_pub, data_pub)
        if start_heartbeat:
            stop_event = threading.Event()
            self.keep_heartbeat[src_name] = stop_event
            threading.Thread(target=self._heartbeat_thread, args=(info_pub, info_text, stop_event), daemon=True).start()
        return info_pub, data_pub

    def _heartbeat_thread(self, info_pub, info_text, stop_event):
        msg = String(data=info_text)
        while not stop_event.is_set():
            info_pub.publish(msg)
            time.sleep(0.5)

    def stop_heartbeat(self, src_name):
        if src_name in self.keep_heartbeat:
            self.keep_heartbeat[src_name].set()

    def publish_data(self, src_name, data_str):
        if src_name in self.pubs:
            self.pubs[src_name][1].publish(String(data=data_str))
        else:
            raise ValueError(f"Unknown source {src_name}")

    def call_snapshot(self, timeout=2.0):
        req = GetSnapshot.Request()
        future = self.snapshot_client.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done():
            result = future.result()
            if result is not None:
                try:
                    return json.loads(result.snapshot_json)
                except json.JSONDecodeError as e:
                    self.get_logger().error(f"JSON error: {e}")
                    return None
        return None

    def destroy_source(self, src_name):
        """彻底销毁输入源：停止心跳 + 销毁发布者 + 移除引用"""
        self.stop_heartbeat(src_name)
        if src_name in self.pubs:
            info_pub, data_pub = self.pubs[src_name]
            self.destroy_publisher(info_pub)   # 从 ROS 图中移除话题
            self.destroy_publisher(data_pub)
            del self.pubs[src_name]


class TestRunner:
    def __init__(self):
        self.tester = None
        self.executor = None
        self.mgmt_process = None

    def setUp(self):
        self.mgmt_process = subprocess.Popen(
            ['ros2', 'run', 'cs_input', 'input_mgmt_node',
             '--ros-args', '-p', f'agent_name:={AGENT_NAME}',
             '-p', f'info_timeout:={INFO_TIMEOUT}',
             '-p', f'discovery_period:={DISCOVERY_PERIOD}'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        time.sleep(2.0)
        rclpy.init()
        self.tester = InputTester()
        self.executor = rclpy.executors.SingleThreadedExecutor()
        self.executor.add_node(self.tester)
        self.tester.wait_for_service()
        print("Waiting for discovery + DDS ...")
        time.sleep(SETTLE_TIME)
        print("Setup complete.\n")

    def tearDown(self):
        self.clear_all_sources()
        if self.tester:
            self.executor.remove_node(self.tester)
            self.tester.destroy_node()
        rclpy.shutdown()
        if self.mgmt_process:
            self.mgmt_process.terminate()
            try:
                self.mgmt_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.mgmt_process.kill()
                self.mgmt_process.wait()

    def spin_for(self, duration):
        deadline = time.time() + duration
        while time.time() < deadline:
            self.executor.spin_once(timeout_sec=0.05)

    def clear_all_sources(self):
        # 消费一次快照清空累积数据
        self.tester.call_snapshot()
        # 销毁所有源（停止心跳 + 销毁发布者）
        for name in list(self.tester.pubs.keys()):
            self.tester.destroy_source(name)
        # 等待 DDS 完全移除话题 + 管理节点超时清理
        time.sleep(1.5)           # DDS 清理缓冲
        self.spin_for(INFO_TIMEOUT + 2.0)
        # 最终确认
        snap = self.tester.call_snapshot()
        assert snap == {}, f"残留: {snap}"

    # ---------- 测试用例 ----------
    def test_basic_accumulate_and_clear(self):
        print("\n=== 测试1: 单源累积与清空 ===")
        src = "sensor_a"
        self.tester.create_input_source(src, "温度传感器")
        self.spin_for(1.5)  # 确保发现 + DDS 匹配

        self.tester.publish_data(src, "temp=25")
        time.sleep(0.1)
        self.spin_for(0.8)
        snap = self.tester.call_snapshot()
        print(f"  快照内容: {json.dumps(snap, ensure_ascii=False)}")
        assert snap is not None and src in snap and snap[src] == ["temp=25"]
        print("  ✓ 第一条数据正确返回")

        self.tester.publish_data(src, "temp=26")
        time.sleep(0.02)
        self.tester.publish_data(src, "temp=27")
        time.sleep(0.1)
        self.spin_for(1.2)
        snap = self.tester.call_snapshot()
        print(f"  快照内容: {json.dumps(snap, ensure_ascii=False)}")
        assert snap[src] == ["temp=26", "temp=27"], f"累积错误: {snap}"
        print("  ✓ 累积两条数据正确")

        snap = self.tester.call_snapshot()
        print(f"  空快照: {json.dumps(snap, ensure_ascii=False)}")
        assert snap == {}, f"清空失败: {snap}"
        print("  ✓ 缓存清空验证通过")

        self.tester.destroy_source(src)

    def test_multiple_sources(self):
        print("\n=== 测试2: 多源并行 ===")
        self.tester.create_input_source("cam_front", "前视")
        self.tester.create_input_source("cam_rear", "后视")
        self.spin_for(1.5)

        self.tester.publish_data("cam_front", "f1")
        time.sleep(0.1)
        self.tester.publish_data("cam_rear", "r1")
        time.sleep(0.1)
        self.spin_for(1.2)
        snap = self.tester.call_snapshot()
        print(f"  快照内容: {json.dumps(snap, ensure_ascii=False)}")
        assert "cam_front" in snap and "cam_rear" in snap
        assert snap["cam_front"] == ["f1"]
        assert snap["cam_rear"] == ["r1"]
        print("  ✓ 多源数据独立正确")

        self.tester.destroy_source("cam_front")
        self.tester.destroy_source("cam_rear")

    def test_heartbeat_timeout(self):
        print("\n=== 测试3: 心跳超时移除 ===")
        src = "temp_src"
        self.tester.create_input_source(src, "临时")
        self.spin_for(1.5)
        self.tester.publish_data(src, "before_timeout")
        self.spin_for(0.8)
        self.tester.stop_heartbeat(src)

        snap = self.tester.call_snapshot()
        print(f"  停止心跳后立即调用快照: {json.dumps(snap, ensure_ascii=False)}")
        assert src in snap
        print("  停止心跳后仍在线 ✓")

        self.spin_for(INFO_TIMEOUT + 1.5)
        snap = self.tester.call_snapshot()
        print(f"  超时后快照: {json.dumps(snap, ensure_ascii=False)}")
        assert src not in snap, f"超时后仍存在: {snap}"
        print("  超时已移除 ✓")

        self.tester.destroy_source(src)

    def test_no_data_no_appear(self):
        print("\n=== 测试4: 无数据不出现 ===")
        src = "no_data"
        self.tester.create_input_source(src, "空")
        self.spin_for(1.5)
        snap = self.tester.call_snapshot()
        print(f"  快照内容: {json.dumps(snap, ensure_ascii=False)}")
        assert src not in snap, f"出现意外: {snap}"
        print("  ✓ 无数据源不出现")

        self.tester.destroy_source(src)

    def test_json_escaping(self):
        print("\n=== 测试5: JSON转义 ===")
        src = "esc"
        self.tester.create_input_source(src, "转义")
        self.spin_for(1.5)
        tricky = 'say "hello"\nC:\\path\nnewline'
        self.tester.publish_data(src, tricky)
        self.spin_for(0.8)
        snap = self.tester.call_snapshot()
        print(f"  快照内容: {json.dumps(snap, ensure_ascii=False)}")
        assert snap[src][0] == tricky, f"转义失败: {snap[src][0]!r}"
        print("  ✓ 特殊字符转义正确")

        self.tester.destroy_source(src)

    def test_special_name(self):
        print("\n=== 测试6: 源名含下划线 ===")
        src = "src_with_underscore"
        self.tester.create_input_source(src, "下线")
        self.spin_for(1.5)
        self.tester.publish_data(src, "d1")
        self.spin_for(0.8)
        snap = self.tester.call_snapshot()
        print(f"  快照内容: {json.dumps(snap, ensure_ascii=False)}")
        assert src in snap
        print("  ✓ 下划线源名正常")

        self.tester.destroy_source(src)

    def test_rapid_consecutive(self):
        print("\n=== 测试7: 连续快照清空 ===")
        # 确保清理干净后再开始
        self.clear_all_sources()
        src = "rapid"
        self.tester.create_input_source(src, "快速")
        self.spin_for(2.0)  # 充足发现时间
        self.tester.publish_data(src, "r1")
        time.sleep(0.1)
        self.spin_for(1.2)

        snap1 = self.tester.call_snapshot()
        print("  snap1:", snap1)
        snap2 = self.tester.call_snapshot()
        print("  snap2:", snap2)
        assert snap1 == {src: ["r1"]}, f"snap1 不符: {snap1}"
        assert snap2 == {}, f"snap2 应为空: {snap2}"
        print("  ✓ 连续调用清空逻辑正确")

        self.tester.destroy_source(src)

    def test_large_volume(self):
        print("\n=== 测试8: 大数据量 ===")
        self.clear_all_sources()
        src = "bulk"
        self.tester.create_input_source(src, "大批量")
        self.spin_for(2.0)  # 充分发现

        count = 200
        print(f"  发送 {count} 条消息...")
        for i in range(count):
            self.tester.publish_data(src, f"m{i}")
            if i % 50 == 0:
                time.sleep(0.01)   # 给回调喘息
                self.spin_for(0.02)
        # 充分 spin 确保所有消息都被管理节点接收
        self.spin_for(5.0)

        snap = self.tester.call_snapshot()
        print(f"  快照包含的源: {list(snap.keys())}" if snap else "  快照为空")
        if src in snap:
            received = len(snap[src])
            print(f"  收到数据条数: {received}")
            if received == count:
                print(f"  前5条: {snap[src][:5]}")
                print(f"  后5条: {snap[src][-5:]}")
            else:
                print(f"  期望 {count} 条，实际 {received} 条")
                print(f"  实际数据前10条: {snap[src][:10]}")
        else:
            print("  ERROR: 源未出现在快照中")
        assert src in snap, f"源未出现: {snap}"
        assert len(snap[src]) == count, f"数量错误: 期望{count}, 实际{len(snap[src])}"
        for i in range(count):
            assert snap[src][i] == f"m{i}", f"第 {i} 条数据错误"
        print("  ✓ 大数据量累积与顺序正确")

        # 确认清空
        snap = self.tester.call_snapshot()
        print(f"  清空后快照: {json.dumps(snap, ensure_ascii=False)}")
        assert snap == {}
        print("  ✓ 大数据量后清空正常")

        self.tester.destroy_source(src)


def main():
    runner = TestRunner()
    try:
        runner.setUp()
        runner.test_basic_accumulate_and_clear()
        runner.test_multiple_sources()
        runner.test_heartbeat_timeout()
        runner.test_no_data_no_appear()
        runner.test_json_escaping()
        runner.test_special_name()
        runner.test_rapid_consecutive()
        runner.test_large_volume()
        print("\n" + "="*40)
        print("全部测试通过！")
        print("="*40)
    except Exception as e:
        print("\n测试失败:", e)
        import traceback
        traceback.print_exc()
    finally:
        runner.tearDown()

if __name__ == '__main__':
    main()