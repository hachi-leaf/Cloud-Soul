#!/usr/bin/env python3
"""
message_send_node 集成测试
测试 topic 单次接收、邮件发送等。
"""
import rclpy, subprocess, time, json, signal, sys, os
from threading import Thread
from rclpy.node import Node
from rclpy.action import ActionClient
from rclpy.executors import MultiThreadedExecutor
from cs_interfaces.action import ExecuteTool
from cs_interfaces.srv import GetToolsInfo
from std_msgs.msg import String

AGENT_NAME = "test_agent"
TOOL_NAME = "message_send"
OUTPUT_PREFIX = f"/{AGENT_NAME}/output"
INFO_SRV = f"{OUTPUT_PREFIX}/info"
INFO_TIMEOUT = 3.0
DISCOVERY_PERIOD = 1.0

TOPIC_OUTPUT = f"/{AGENT_NAME}/output/message_send/raw_message"

def force_clean():
    for name in ["output_mgmt_node", "message_send_node", "file_read_node", "file_write_node", "shell_exec_node"]:
        subprocess.run(["pkill", "-9", "-f", name], stderr=subprocess.DEVNULL, timeout=2)

def kill_proc_tree(p):
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except ProcessLookupError:
        pass

class TestClient(Node):
    def __init__(self):
        super().__init__("test_client")
        self.info_cli = self.create_client(GetToolsInfo, INFO_SRV)
        self.action_cli = ActionClient(self, ExecuteTool, OUTPUT_PREFIX)
        self.received_messages = []
        self.topic_sub = self.create_subscription(
            String, TOPIC_OUTPUT,
            lambda msg: self.received_messages.append(msg.data), 10)
        time.sleep(0.5)

    def call_info(self, timeout=5.0):
        if not self.info_cli.wait_for_service(timeout):
            raise RuntimeError("信息服务不可用")
        req = GetToolsInfo.Request()
        future = self.info_cli.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done(): return future.result().tools_json
        raise RuntimeError("信息服务调用超时")

    def send_tool(self, tool_name, arguments, timeout_sec=0.0, wait=20.0):
        goal = ExecuteTool.Goal()
        goal.input_json = json.dumps({"name": tool_name, "arguments": arguments}, ensure_ascii=False)
        goal.timeout_sec = float(timeout_sec)
        if not self.action_cli.wait_for_server(5.0): raise RuntimeError("动作服务器未就绪")
        send_future = self.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self, send_future, timeout_sec=5.0)
        if not send_future.done(): raise RuntimeError("发送目标超时")
        gh = send_future.result()
        if not gh.accepted: return -99, '{"error":"拒绝"}', "abort"
        rf = gh.get_result_async()
        rclpy.spin_until_future_complete(self, rf, timeout_sec=wait)
        if not rf.done():
            gh.cancel_goal_async()
            return -100, '{"error":"客户端等待超时"}', "timeout"
        res = rf.result()
        return res.result.exit_code, res.result.output_json, "succeed"

    def wait_for_tool(self, tool_name, present=True, timeout=15.0):
        start = time.time()
        while time.time() - start < timeout:
            try:
                tools = json.loads(self.call_info())
            except: tools = []
            names = [f["function"]["name"] for f in tools if "function" in f]
            if (present and tool_name in names) or (not present and tool_name not in names): return True
            time.sleep(0.3)
        return False

class Tester:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.manager_proc = None
        self.tool_proc = None
        self.test_cli = None
        self.executor = None
        self.thread = None

    def log(self, msg): print(f"[测试] {msg}")
    def assert_true(self, cond, msg):
        if cond:
            self.passed += 1; self.log(f"✅ 通过: {msg}")
        else:
            self.failed += 1; self.log(f"❌ 失败: {msg}")
    def assert_equal(self, a, b, msg):
        self.assert_true(a == b, f"{msg} (期望 {b}，实际 {a})")

    def start_nodes(self):
        self.log("启动 output_mgmt_node")
        self.manager_proc = subprocess.Popen(
            ["ros2","run","cs_output","output_mgmt_node","--ros-args",
             "-p",f"agent_name:={AGENT_NAME}",
             "-p",f"info_timeout:={INFO_TIMEOUT}",
             "-p",f"discovery_period:={DISCOVERY_PERIOD}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True)
        time.sleep(2)

        self.log("启动 message_send_node")
        cmd = ["ros2","run","cs_output","message_send_node","--ros-args",
               "-p",f"agent_name:={AGENT_NAME}"]
        self.tool_proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True)
        time.sleep(2)

        topic = f"/{AGENT_NAME}/output/{TOOL_NAME}/info"
        topics = subprocess.check_output(["ros2","topic","list"], text=True)
        if topic not in topics:
            self.log(f"❌ 话题 {topic} 不存在")
            return False
        return True

    def stop_nodes(self):
        for proc in (self.tool_proc, self.manager_proc):
            if proc is None or proc.poll() is not None: continue
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGINT)
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                kill_proc_tree(proc)
                try: proc.wait(timeout=2)
                except subprocess.TimeoutExpired: pass
            except ProcessLookupError: pass
        self.manager_proc = None
        self.tool_proc = None
        force_clean()

    def start_rclpy(self):
        rclpy.init()
        self.test_cli = TestClient()
        self.executor = MultiThreadedExecutor()
        self.executor.add_node(self.test_cli)
        self.thread = Thread(target=self.executor.spin, daemon=True)
        self.thread.start()
        time.sleep(1)

    def shutdown_rclpy(self):
        if self.executor: self.executor.shutdown()
        if self.test_cli: self.test_cli.destroy_node()
        rclpy.shutdown()
        if self.thread: self.thread.join(timeout=3)

    def run_tests(self):
        self.log("========== message_send 测试 ==========")
        self.log("等待工具被发现...")
        if not self.test_cli.wait_for_tool(TOOL_NAME):
            self.assert_true(False, "工具未发现"); return

        # 测试 1: topic 渠道发送消息（验证只出现一次）
        self.log("测试 1: topic 渠道发送消息并验证不会重复接收")
        # 清空之前可能接收到的消息（如果有 transient_local 残留，这里清掉便于验证）
        self.test_cli.received_messages.clear()
        test_msg = "唯一消息_" + str(time.time())
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {
            "channel": "topic", "message": test_msg
        })
        self.log(f"  输出: {out}")
        self.assert_equal(code, 0, "退出码 0")
        self.assert_true('"status":"published"' in out, "状态为 published")

        # 等待消息到达
        time.sleep(1)
        count = sum(1 for m in self.test_cli.received_messages if test_msg in m)
        self.assert_equal(count, 1, f"消息出现次数为 1 (实际 {count})")

        # 等待 2 秒，再次检查是否有新的相同消息出现（应无重复）
        time.sleep(2)
        count_after = sum(1 for m in self.test_cli.received_messages if test_msg in m)
        self.assert_equal(count_after, 1, "消息未重复出现")

        # 测试 2: topic 渠道缺少 message
        self.log("测试 2: topic 渠道缺少 message")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"channel": "topic"})
        self.log(f"  输出: {out}")
        self.assert_equal(code, -1, "退出码 -1 (参数错误)")

        # 测试 3: 邮件发送（依赖 ~/.mailrc）
        self.log("测试 3: 真实邮件发送（发送给自己）")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {
            "channel": "email",
            "to": "zxy_yys_leaf@163.com",          # 你的邮箱
            "subject": "CloudSoul 测试邮件",
            "body": "这封邮件由 message_send_node 发送，验证 s‑nail 正常工作。"
        }, timeout_sec=30.0, wait=30.0)
        self.log(f"  输出: {out}")
        if code == 0:
            self.assert_true('"status":"sent"' in out, "邮件发送成功")
        else:
            self.log("  邮件发送失败，请检查 s‑nail 和 ~/.mailrc 配置")
            self.assert_true(False, f"邮件发送失败，exit_code={code}")

        # 测试 4: 不支持的渠道
        self.log("测试 4: 不支持的渠道")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"channel": "wechat", "message": "x"})
        self.log(f"  输出: {out}")
        self.assert_equal(code, -2, "退出码 -2 (不支持)")

        # 测试 5: 缺少 channel
        self.log("测试 5: 缺少 channel")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"message": "x"})
        self.log(f"  输出: {out}")
        self.assert_equal(code, -1, "退出码 -1 (缺少 channel)")

        self.log(f"========== 结果：通过 {self.passed}，失败 {self.failed} ==========")

def main():
    force_clean()
    time.sleep(1)
    tester = Tester()
    try:
        if not tester.start_nodes(): sys.exit(1)
        tester.start_rclpy()
        tester.run_tests()
    except Exception as e:
        print(f"异常: {e}")
        import traceback; traceback.print_exc()
        tester.failed += 1
    finally:
        tester.shutdown_rclpy()
        tester.stop_nodes()
    return 0 if tester.failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())