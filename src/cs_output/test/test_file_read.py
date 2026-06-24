# Copyright (c) leaf
# SPDX-License-Identifier: MIT

#!/usr/bin/env python3
"""
file_read_node 集成测试（真取消版，大文件 50MB）
"""
import rclpy, subprocess, time, json, signal, sys, os, tempfile
from threading import Thread
from rclpy.node import Node
from rclpy.action import ActionClient
from rclpy.executors import MultiThreadedExecutor
from cs_interfaces.action import ExecuteTool
from cs_interfaces.srv import GetToolsInfo

AGENT_NAME = "test_agent"
TOOL_NAME = "file_read_node"
OUTPUT_PREFIX = f"/{AGENT_NAME}/output"
INFO_SRV = f"{OUTPUT_PREFIX}/info"
INFO_TIMEOUT = 3.0
DISCOVERY_PERIOD = 1.0

# 大文件尺寸：50 MB（确保取消时仍在读取）
BIG_FILE_SIZE_KB = 50 * 1024   # 50 MB

def force_clean():
    for name in ["output_mgmt_node", "file_read_node", "file_write_node", "shell_exec_node", "mock_tool_node"]:
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

    def call_info(self, timeout=5.0):
        if not self.info_cli.wait_for_service(timeout): raise RuntimeError("信息服务不可用")
        req = GetToolsInfo.Request()
        future = self.info_cli.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done(): return future.result().tools_json
        raise RuntimeError("信息服务调用超时")

    def send_tool(self, tool_name, arguments, timeout_sec=0.0, wait=20.0):
        goal = ExecuteTool.Goal()
        goal.input_json = json.dumps({"name": tool_name, "arguments": arguments})
        goal.timeout_sec = float(timeout_sec)
        if not self.action_cli.wait_for_server(5.0): raise RuntimeError("动作服务器未就绪")
        send_future = self.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self, send_future, timeout_sec=5.0)
        if not send_future.done(): raise RuntimeError("发送目标超时")
        gh = send_future.result()
        if not gh.accepted: return -99, '{"error":"拒绝"}', "abort"
        self._last_gh = gh
        rf = gh.get_result_async()
        rclpy.spin_until_future_complete(self, rf, timeout_sec=wait)
        if not rf.done():
            gh.cancel_goal_async()
            return -100, '{"error":"等待超时"}', "timeout"
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
        self.tmpfile = None

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

        self.log("启动 file_read_node")
        self.tool_proc = subprocess.Popen(
            ["ros2","run","cs_output","file_read_node","--ros-args",
             "-p",f"agent_name:={AGENT_NAME}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
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
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
            except ProcessLookupError:
                pass
        self.manager_proc = None
        self.tool_proc = None
        force_clean()
        if self.tmpfile and os.path.exists(self.tmpfile):
            os.unlink(self.tmpfile)

    def create_temp_file(self, content="Hello World\n测试内容\n", size_kb=0):
        fd, path = tempfile.mkstemp(suffix=".txt", prefix="cs_test_read_")
        os.close(fd)
        if size_kb > 0:
            content = "A" * 1024 * size_kb
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)
        self.tmpfile = path
        return path

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
        self.log("========== file_read 测试 ==========")
        self.log("等待工具被发现...")
        if not self.test_cli.wait_for_tool(TOOL_NAME):
            self.assert_true(False, "工具未发现"); return

        # 1. 正常读取
        self.log("测试 1: 正常读取文件")
        path = self.create_temp_file("Hello文件读取\n第二行")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"path": path})
        self.log(f"  输出: {out}")
        self.assert_equal(code, 0, "退出码 0")
        try:
            data = json.loads(out)
            self.assert_true("Hello文件读取" in data["content"], "内容包含中文")
        except: self.assert_true(False, "JSON 非法")

        # 2. 部分读取
        self.log("测试 2: 带 offset 和 length")
        path = self.create_temp_file("0123456789")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"path": path, "offset": 3, "length": 2})
        self.log(f"  输出: {out}")
        self.assert_equal(code, 0, "退出码 0")
        data = json.loads(out)
        self.assert_equal(data.get("content",""), "34", "内容为 '34'")

        # 3. 读取不存在的文件
        self.log("测试 3: 读取不存在的文件")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"path": "/tmp/nonexistent_xyz"})
        self.assert_equal(code, -2, "退出码 -2")

        # 4. 非法输入
        self.log("测试 4: 缺少 path")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {})
        self.assert_equal(code, -1, "退出码 -1")

        # 5. 大文件读取并取消（必须触发取消）
        self.log(f"测试 5: 大文件读取时取消（{BIG_FILE_SIZE_KB//1024} MB）")
        path = self.create_temp_file(size_kb=BIG_FILE_SIZE_KB)
        goal = ExecuteTool.Goal()
        goal.input_json = json.dumps({"name": TOOL_NAME, "arguments": {"path": path}})
        goal.timeout_sec = 0.0
        send_future = self.test_cli.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self.test_cli, send_future, timeout_sec=5.0)
        gh = send_future.result()
        self.assert_true(gh.accepted, "目标接受")
        time.sleep(0.3)   # 稍等片刻让读取开始
        cancel_future = gh.cancel_goal_async()
        rclpy.spin_until_future_complete(self.test_cli, cancel_future, timeout_sec=5.0)
        self.log("  已发送取消请求")
        rf = gh.get_result_async()
        rclpy.spin_until_future_complete(self.test_cli, rf, timeout_sec=15.0)
        if rf.done():
            res = rf.result()
            code = res.result.exit_code
            try:
                data = json.loads(res.result.output_json)
            except:
                data = {}
            size = data.get("size", "?")
            err = data.get("error", "")
            self.log(f"  结果: exit_code={code}, error={err}, size={size}")
            self.assert_equal(code, -7, "取消退出码 -7")
            self.assert_true("execution canceled" in res.result.output_json, "包含取消信息")
        else:
            self.assert_true(False, "未收到结果")

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