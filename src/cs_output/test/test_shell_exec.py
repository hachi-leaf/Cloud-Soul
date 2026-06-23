#!/usr/bin/env python3
"""
shell_exec_node 集成测试
"""
import rclpy, subprocess, time, json, signal, sys, os
from threading import Thread
from rclpy.node import Node
from rclpy.action import ActionClient
from rclpy.executors import MultiThreadedExecutor
from cs_interfaces.action import ExecuteTool
from cs_interfaces.srv import GetToolsInfo

AGENT_NAME = "test_agent"
TOOL_NAME = "shell_exec_node"
OUTPUT_PREFIX = f"/{AGENT_NAME}/output"
INFO_SRV = f"{OUTPUT_PREFIX}/info"
INFO_TIMEOUT = 3.0
DISCOVERY_PERIOD = 1.0

# ---------- 工具函数 ----------
def force_clean():
    """彻底清除可能残留的所有测试节点"""
    for name in ["output_mgmt_node", "shell_exec_node", "file_read_node", "file_write_node", "mock_tool_node"]:
        subprocess.run(["pkill", "-9", "-f", name], stderr=subprocess.DEVNULL, timeout=2)

def kill_proc_tree(p):
    """强制杀死进程及其所有子进程（通过进程组）"""
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except ProcessLookupError:
        pass

# ---------- 测试客户端 ----------
class TestClient(Node):
    def __init__(self):
        super().__init__("test_client")
        self.info_cli = self.create_client(GetToolsInfo, INFO_SRV)
        self.action_cli = ActionClient(self, ExecuteTool, OUTPUT_PREFIX)

    def call_info(self, timeout=5.0):
        if not self.info_cli.wait_for_service(timeout):
            raise RuntimeError("信息服务不可用")
        req = GetToolsInfo.Request()
        future = self.info_cli.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done():
            return future.result().tools_json
        raise RuntimeError("信息服务调用超时")

    def send_tool(self, tool_name, arguments, timeout_sec=0.0, wait=20.0):
        goal = ExecuteTool.Goal()
        goal.input_json = json.dumps({"name": tool_name, "arguments": arguments})
        goal.timeout_sec = float(timeout_sec)
        if not self.action_cli.wait_for_server(5.0):
            raise RuntimeError("动作服务器未就绪")
        send_future = self.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self, send_future, timeout_sec=5.0)
        if not send_future.done():
            raise RuntimeError("发送目标超时")
        goal_handle = send_future.result()
        if not goal_handle.accepted:
            return -99, '{"error":"目标被拒绝"}', "abort"
        self._last_goal_handle = goal_handle
        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(self, result_future, timeout_sec=wait)
        if not result_future.done():
            goal_handle.cancel_goal_async()
            return -100, '{"error":"客户端等待超时"}', "timeout"
        res = result_future.result()
        return res.result.exit_code, res.result.output_json, "succeed"

    def cancel_last(self):
        if hasattr(self, '_last_goal_handle'):
            self._last_goal_handle.cancel_goal_async()

    def wait_for_tool(self, tool_name, present=True, timeout=15.0):
        start = time.time()
        while time.time() - start < timeout:
            try:
                tools = json.loads(self.call_info())
            except:
                tools = []
            names = [f["function"]["name"] for f in tools if "function" in f]
            if (present and tool_name in names) or (not present and tool_name not in names):
                return True
            time.sleep(0.3)
        return False

# ---------- 测试执行器 ----------
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
            self.passed += 1
            self.log(f"✅ 通过: {msg}")
        else:
            self.failed += 1
            self.log(f"❌ 失败: {msg}")
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
            start_new_session=True)   # ★ 创建独立会话组
        time.sleep(2)

        self.log("启动 shell_exec_node")
        self.tool_proc = subprocess.Popen(
            ["ros2","run","cs_output","shell_exec_node","--ros-args",
             "-p",f"agent_name:={AGENT_NAME}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True)   # ★ 创建独立会话组
        time.sleep(2)

        # 检查话题
        topic = f"/{AGENT_NAME}/output/{TOOL_NAME}/info"
        topics = subprocess.check_output(["ros2","topic","list"], text=True)
        if topic not in topics:
            self.log(f"❌ 话题 {topic} 不存在")
            return False
        return True

    def stop_nodes(self):
        """强制结束所有节点进程（包括子进程）"""
        for proc in (self.tool_proc, self.manager_proc):
            if proc is None or proc.poll() is not None:
                continue
            try:
                # 先尝试优雅终止整个进程组
                os.killpg(os.getpgid(proc.pid), signal.SIGINT)
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                # 超时则强制杀死整个进程组
                kill_proc_tree(proc)
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
            except ProcessLookupError:
                pass
        self.manager_proc = None
        self.tool_proc = None
        # 兜底清理
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
        self.log("========== shell_exec 测试 ==========")
        self.log("等待工具被发现...")
        if not self.test_cli.wait_for_tool(TOOL_NAME):
            self.assert_true(False, "工具未发现")
            return

        # 测试1：正常执行
        self.log("测试 1: 正常执行 'echo hello'")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"command": "echo hello"}, timeout_sec=5.0)
        self.log(f"  输出: {out}")
        self.assert_equal(code, 0, "退出码 0")
        try:
            data = json.loads(out)
            self.assert_true("hello" in data["stdout"], "包含 hello")
        except:
            self.assert_true(False, "返回 JSON 非法")

        # 测试2：非零退出
        self.log("测试 2: 命令返回非零退出码 'exit 42'")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"command": "exit 42"}, timeout_sec=5.0)
        self.log(f"  输出: {out}")
        self.assert_equal(code, -8, "退出码 -8")
        try:
            data = json.loads(out)
            self.assert_equal(data["exit_code"], 42, "实际退出码 42")
        except:
            self.assert_true(False, "返回 JSON 非法")

        # 测试3：超时取消
        self.log("测试 3: 超时取消 (sleep 10, timeout_sec=2)")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"command": "sleep 10"}, timeout_sec=2.0, wait=10.0)
        self.log(f"  输出: {out}")
        self.assert_equal(code, -7, "超时退出码 -7")
        self.assert_true("tool execution timeout" in out, "包含超时信息")

        # 测试4：主动取消
        self.log("测试 4: 主动取消 (sleep 20)")
        goal = ExecuteTool.Goal()
        goal.input_json = json.dumps({"name": TOOL_NAME, "arguments": {"command": "sleep 20"}})
        goal.timeout_sec = 0.0
        send_future = self.test_cli.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self.test_cli, send_future, timeout_sec=5.0)
        gh = send_future.result()
        self.assert_true(gh.accepted, "目标接受")
        time.sleep(0.5)
        cancel_future = gh.cancel_goal_async()
        rclpy.spin_until_future_complete(self.test_cli, cancel_future, timeout_sec=5.0)
        self.log("  已发送取消请求")
        result_future = gh.get_result_async()
        rclpy.spin_until_future_complete(self.test_cli, result_future, timeout_sec=10.0)
        if result_future.done():
            res = result_future.result()
            self.log(f"  输出: {res.result.output_json}")
            self.assert_equal(res.result.exit_code, -7, "取消退出码 -7")
            ok = "execution canceled" in res.result.output_json or "tool execution timeout" in res.result.output_json
            self.assert_true(ok, "包含取消或超时信息")
        else:
            self.assert_true(False, "未收到取消结果")

        # 测试5：缺少 command
        self.log("测试 5: 缺少 command")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {}, timeout_sec=5.0)
        self.log(f"  输出: {out}")
        self.assert_equal(code, -1, "退出码 -1 (解析错误)")
        self.assert_true("missing command" in out or "invalid input json" in out, "包含错误描述")

def main():
    # ★ 启动前强制清理残留
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
        tester.stop_nodes()   # 内部已包含强制清理
    return 0 if tester.failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())