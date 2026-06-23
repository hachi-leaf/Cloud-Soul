#!/usr/bin/env python3
"""
output_mgmt_node 功能测试脚本（中文版，修复断言空格问题）
自动启动管理节点、模拟工具节点，测试所有功能。
"""

import rclpy
import subprocess
import time
import json
import signal
import sys
from threading import Thread
from rclpy.node import Node
from rclpy.action import ActionServer, ActionClient
from rclpy.executors import MultiThreadedExecutor
from rclpy.callback_groups import ReentrantCallbackGroup
from cs_interfaces.action import ExecuteTool
from cs_interfaces.srv import GetToolsInfo
from std_msgs.msg import String

# ---------- 测试参数 ----------
AGENT_NAME = "test_agent"
TOOL_NAME = "test_tool"
OUTPUT_PREFIX = f"/{AGENT_NAME}/output"
INFO_TOPIC = f"{OUTPUT_PREFIX}/{TOOL_NAME}/info"
ACTION_NAME = f"{OUTPUT_PREFIX}/{TOOL_NAME}"
INFO_SRV = f"{OUTPUT_PREFIX}/info"
INFO_TIMEOUT = 3.0
DISCOVERY_PERIOD = 1.0

# ---------- 模拟工具节点 ----------
class MockToolNode(Node):
    def __init__(self):
        super().__init__("mock_tool_node")
        self.info_pub = self.create_publisher(
            String, INFO_TOPIC,
            qos_profile=rclpy.qos.QoSProfile(
                reliability=rclpy.qos.ReliabilityPolicy.RELIABLE,
                durability=rclpy.qos.DurabilityPolicy.TRANSIENT_LOCAL,
                depth=1))
        tool_desc = {
            "name": TOOL_NAME,
            "description": "测试工具，验证管理节点功能",
            "parameters": {
                "type": "object",
                "properties": {"value": {"type": "integer"}},
                "required": ["value"]
            }
        }
        self.info_json = json.dumps(tool_desc)
        self._action_server = ActionServer(
            self, ExecuteTool, ACTION_NAME,
            self.execute_callback,
            callback_group=ReentrantCallbackGroup())
        self.action_mode = "success"   # success / timeout / abort
        self.return_echo = True
        self.pub_timer = self.create_timer(0.5, self.publish_info)
        self.publish_info()

    def publish_info(self):
        msg = String()
        msg.data = self.info_json
        self.info_pub.publish(msg)

    async def execute_callback(self, goal_handle):
        self.get_logger().info(f"工具收到请求: {goal_handle.request.input_json}")
        if self.action_mode == "timeout":
            await rclpy.task.Future()   # 永不完成，模拟超时
            return
        if self.action_mode == "abort":
            goal_handle.abort()
            result = ExecuteTool.Result()
            result.output_json = '{"error":"工具内部异常中止"}'
            result.exit_code = 42
            return result

        goal_handle.succeed()
        result = ExecuteTool.Result()
        if self.return_echo:
            try:
                args = json.loads(goal_handle.request.input_json)
                result.output_json = json.dumps({"received": args, "status": "ok"})
                self.get_logger().info(f"工具返回结果: {result.output_json}")
            except Exception as e:
                self.get_logger().error(f"工具解析参数失败: {e}")
                result.output_json = '{"echo":"invalid json"}'
            result.exit_code = 0
        else:
            result.output_json = '{"result":"custom"}'
            result.exit_code = 7
        return result

    def stop_publishing(self):
        self.pub_timer.cancel()

    def start_publishing(self):
        self.pub_timer.reset()
        self.publish_info()

# ---------- 测试客户端 ----------
class TestClientNode(Node):
    def __init__(self):
        super().__init__("test_client_node")
        self.info_cli = self.create_client(GetToolsInfo, INFO_SRV)
        self.action_cli = ActionClient(self, ExecuteTool, f"{OUTPUT_PREFIX}")

    def call_info_service(self, timeout=5.0):
        if not self.info_cli.wait_for_service(timeout):
            raise RuntimeError("信息服务不可用")
        req = GetToolsInfo.Request()
        future = self.info_cli.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done():
            return future.result().tools_json
        raise RuntimeError("信息服务调用超时")

    def send_tool_call(self, tool_name, arguments, timeout_sec=0.0, wait_timeout=10.0):
        goal = ExecuteTool.Goal()
        goal.input_json = json.dumps({"name": tool_name, "arguments": arguments})
        goal.timeout_sec = float(timeout_sec)

        if not self.action_cli.wait_for_server(5.0):
            raise RuntimeError("统一动作服务器 /output 未就绪")
        send_future = self.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self, send_future, timeout_sec=5.0)
        if not send_future.done():
            raise RuntimeError("发送目标超时")
        goal_handle = send_future.result()
        if not goal_handle.accepted:
            return -99, '{"error":"目标被管理节点拒绝"}', "abort"

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(self, result_future, timeout_sec=wait_timeout)
        if not result_future.done():
            goal_handle.cancel_goal_async()
            return -100, '{"error":"客户端等待结果超时"}', "timeout"
        res = result_future.result()
        return res.result.exit_code, res.result.output_json, "succeed"

    def wait_for_tool(self, tool_name, present=True, timeout=10.0):
        """循环检查工具是否出现/消失"""
        start = time.time()
        while time.time() - start < timeout:
            try:
                tools = json.loads(self.call_info_service())
            except Exception:
                tools = []
            tool_names = [f["function"]["name"] for f in tools if "function" in f]
            if (present and tool_name in tool_names) or (not present and tool_name not in tool_names):
                return True
            time.sleep(0.3)
        return False

# ---------- 测试执行器 ----------
class Tester:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.manager = None
        self.mock_node = None
        self.test_cli = None
        self.executor = None
        self.thread = None

    def log(self, msg):
        print(f"[测试] {msg}")

    def assert_true(self, cond, msg):
        if cond:
            self.passed += 1
            self.log(f"✅ 通过: {msg}")
        else:
            self.failed += 1
            self.log(f"❌ 失败: {msg}")

    def assert_equal(self, a, b, msg):
        self.assert_true(a == b, f"{msg} (期望 {b}，实际 {a})")

    def start_manager(self):
        self.log("启动管理节点...")
        cmd = [
            "ros2", "run", "cs_output", "output_mgmt_node",
            "--ros-args",
            "-p", f"agent_name:={AGENT_NAME}",
            "-p", f"info_timeout:={INFO_TIMEOUT}",
            "-p", f"discovery_period:={DISCOVERY_PERIOD}"
        ]
        self.manager = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        time.sleep(3)

    def stop_manager(self):
        if self.manager:
            self.manager.send_signal(signal.SIGINT)
            self.manager.wait()
            self.manager = None

    def start_rclpy(self):
        rclpy.init()
        self.mock_node = MockToolNode()
        self.test_cli = TestClientNode()
        self.executor = MultiThreadedExecutor()
        self.executor.add_node(self.mock_node)
        self.executor.add_node(self.test_cli)
        self.thread = Thread(target=self.executor.spin, daemon=True)
        self.thread.start()
        time.sleep(1.5)

    def shutdown_rclpy(self):
        if self.executor:
            self.executor.shutdown()
        if self.mock_node:
            self.mock_node.destroy_node()
        if self.test_cli:
            self.test_cli.destroy_node()
        rclpy.shutdown()
        if self.thread:
            self.thread.join(timeout=3)

    def run_tests(self):
        self.log("========== 开始测试 ==========")

        # 测试 1：初始工具已在线
        self.log("测试 1：初始工具已发现")
        time.sleep(0.5)
        tools = json.loads(self.test_cli.call_info_service())
        tool_names = [f["function"]["name"] for f in tools if "function" in f]
        self.assert_true(TOOL_NAME in tool_names, "工具列表中包含测试工具")

        # 测试 2：成功调用工具（紧凑 JSON）
        self.log("测试 2：标准工具调用（紧凑 JSON）")
        code, out, _ = self.test_cli.send_tool_call(TOOL_NAME, {"value": 123})
        self.log(f"  实际收到 output_json: {out}")
        self.assert_equal(code, 0, "退出码为 0")
        # 解析返回的 JSON，验证 status 字段
        try:
            data = json.loads(out)
            self.assert_true(data.get("status") == "ok", "返回结果中包含 status: ok")
        except:
            self.assert_true(False, "返回结果不是合法 JSON")

        # 测试 3：输入 JSON 包含空格、换行、制表符（验证空白压缩）
        self.log("测试 3：宽松 JSON 输入（含空白和换行）")
        loose_input = '{\n  "name": \t"test_tool",\n  "arguments":   {"value":  \n999}\n}'
        goal = ExecuteTool.Goal()
        goal.input_json = loose_input
        goal.timeout_sec = 5.0
        send_future = self.test_cli.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self.test_cli, send_future, timeout_sec=5.0)
        goal_handle = send_future.result()
        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(self.test_cli, result_future, timeout_sec=5.0)
        res = result_future.result()
        self.log(f"  实际收到 output_json: {res.result.output_json}")
        self.assert_equal(res.result.exit_code, 0, "空白压缩后退出码为 0")
        try:
            data = json.loads(res.result.output_json)
            self.assert_true(data.get("status") == "ok", "空白压缩后调用成功返回 status: ok")
        except:
            self.assert_true(False, "空白压缩后返回结果不是合法 JSON")

        # 测试 4：管理节点等待超时（工具不响应）
        self.log("测试 4：管理节点超时取消")
        self.mock_node.action_mode = "timeout"
        code, out, _ = self.test_cli.send_tool_call(
            TOOL_NAME, {"value": 1}, timeout_sec=2.0, wait_timeout=5.0)
        self.log(f"  实际收到 output_json: {out}")
        self.assert_equal(code, -7, "超时退出码 -7")
        self.assert_true("tool execution timeout" in out, "错误信息包含超时提示")
        self.mock_node.action_mode = "success"

        # 测试 5：工具自身异常中止
        self.log("测试 5：工具自身异常中止")
        self.mock_node.action_mode = "abort"
        code, out, _ = self.test_cli.send_tool_call(TOOL_NAME, {})
        self.log(f"  实际收到 output_json: {out}")
        # 工具 abort 返回自定义 exit_code=42 和消息
        self.assert_equal(code, 42, "退出码透传为 42")
        self.assert_true("工具内部异常中止" in out, "错误信息透传工具内部异常中止")
        self.mock_node.action_mode = "success"

        # 测试 6：输入 JSON 缺少 name 字段
        self.log("测试 6：非法输入（缺少 name 字段）")
        bad_goal = ExecuteTool.Goal()
        bad_goal.input_json = '{"arguments":{}}'   # 无 name
        bad_goal.timeout_sec = 5.0
        send_future = self.test_cli.action_cli.send_goal_async(bad_goal)
        rclpy.spin_until_future_complete(self.test_cli, send_future, timeout_sec=5.0)
        goal_handle = send_future.result()
        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(self.test_cli, result_future, timeout_sec=5.0)
        res = result_future.result()
        self.log(f"  实际收到 output_json: {res.result.output_json}")
        self.assert_equal(res.result.exit_code, -1, "退出码 -1 (解析错误)")
        self.assert_true("invalid input json" in res.result.output_json, "错误信息提示输入非法")

        # 测试 7：调用不存在的工具
        self.log("测试 7：调用未发现的工具")
        code, out, _ = self.test_cli.send_tool_call("nonexistent", {})
        self.log(f"  实际收到 output_json: {out}")
        self.assert_equal(code, -2, "退出码 -2 (工具未找到)")

        # 测试 8：心跳超时移除工具
        self.log("测试 8：心跳超时移除与恢复")
        self.mock_node.stop_publishing()
        self.log("等待心跳超时...")
        time.sleep(INFO_TIMEOUT + 4.0)
        removed = self.test_cli.wait_for_tool(TOOL_NAME, present=False, timeout=5.0)
        self.assert_true(removed, "工具已从列表中移除")
        self.mock_node.start_publishing()
        appeared = self.test_cli.wait_for_tool(TOOL_NAME, present=True, timeout=5.0)
        self.assert_true(appeared, "工具重新出现")

        # 测试 9：信息服务异常保护（正常情况）
        self.log("测试 9：信息服务稳定性")
        tools = json.loads(self.test_cli.call_info_service())
        self.assert_true(len(tools) > 0, "信息服务可正常返回工具列表")

        # 测试 10：动态超时参数生效
        self.log("测试 10：目标携带动态超时")
        code, out, _ = self.test_cli.send_tool_call(TOOL_NAME, {"value": 456}, timeout_sec=10.0)
        self.log(f"  实际收到 output_json: {out}")
        self.assert_equal(code, 0, "动态超时调用成功")
        try:
            data = json.loads(out)
            self.assert_true(data.get("status") == "ok", "动态超时调用返回 status: ok")
        except:
            self.assert_true(False, "动态超时返回结果不是合法 JSON")

        self.log(f"========== 结果：通过 {self.passed}，失败 {self.failed} ==========")

def main():
    tester = Tester()
    try:
        tester.start_manager()
        tester.start_rclpy()
        tester.run_tests()
    except Exception as e:
        print(f"测试框架异常: {e}")
        import traceback
        traceback.print_exc()
        tester.failed += 1
    finally:
        tester.shutdown_rclpy()
        tester.stop_manager()
    return 0 if tester.failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())