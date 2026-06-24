# Copyright (c) leaf
# SPDX-License-Identifier: MIT

#!/usr/bin/env python3
"""
file_write_node 集成测试（修复取消断言）
"""
import rclpy, subprocess, time, json, signal, sys, os, tempfile
from threading import Thread
from rclpy.node import Node
from rclpy.action import ActionClient
from rclpy.executors import MultiThreadedExecutor
from cs_interfaces.action import ExecuteTool
from cs_interfaces.srv import GetToolsInfo

AGENT_NAME = "test_agent"
TOOL_NAME = "file_write_node"
OUTPUT_PREFIX = f"/{AGENT_NAME}/output"
INFO_SRV = f"{OUTPUT_PREFIX}/info"
INFO_TIMEOUT = 3.0
DISCOVERY_PERIOD = 1.0

BIG_CONTENT_MB = 200   # 200 MB 确保取消时仍在写入

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
        goal.input_json = json.dumps(
            {"name": tool_name, "arguments": arguments},
            ensure_ascii=False)
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

        self.log("启动 file_write_node")
        self.tool_proc = subprocess.Popen(
            ["ros2","run","cs_output","file_write_node","--ros-args",
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
        self.log("========== file_write 测试 ==========")
        self.log("等待工具被发现...")
        if not self.test_cli.wait_for_tool(TOOL_NAME):
            self.assert_true(False, "工具未发现"); return

        # 1. 覆盖写入
        self.log("测试 1: 覆盖写入（含中文）")
        tmp = tempfile.mktemp(suffix=".txt")
        content = "覆盖内容"
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {
            "path": tmp, "content": content, "mode": "overwrite"
        })
        self.log(f"  输出: {out}")
        self.assert_equal(code, 0, "退出码 0")
        with open(tmp, 'r', encoding='utf-8') as f:
            written = f.read()
        self.assert_equal(written, content, "文件内容正确")
        os.unlink(tmp)

        # 2. 追加写入
        self.log("测试 2: 追加写入")
        tmp = tempfile.mktemp(suffix=".txt")
        with open(tmp, 'w', encoding='utf-8') as f: f.write("第一行\n")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {
            "path": tmp, "content": "第二行", "mode": "append"
        })
        self.assert_equal(code, 0, "退出码 0")
        with open(tmp, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assert_true(content == "第一行\n第二行", "追加后内容正确")
        os.unlink(tmp)

        # 3. 非法输入
        self.log("测试 3: 缺少 content")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {"path": "/tmp/x"})
        self.assert_equal(code, -1, "退出码 -1")

        # 4. 写入只读目录
        self.log("测试 4: 写入不可写路径")
        code, out, _ = self.test_cli.send_tool(TOOL_NAME, {
            "path": "/root/test_write.txt", "content": "test"
        })
        self.assert_equal(code, -2, "退出码 -2 (打开失败)")

        # 5. 大内容写入并取消
        self.log(f"测试 5: 大内容写入时取消 ({BIG_CONTENT_MB} MB)")
        big_content = "A" * (BIG_CONTENT_MB * 1024 * 1024)
        tmp = tempfile.mktemp(suffix=".txt")
        goal = ExecuteTool.Goal()
        goal.input_json = json.dumps(
            {"name": TOOL_NAME, "arguments": {"path": tmp, "content": big_content}},
            ensure_ascii=False)
        goal.timeout_sec = 0.0
        send_future = self.test_cli.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self.test_cli, send_future, timeout_sec=5.0)
        gh = send_future.result()
        self.assert_true(gh.accepted, "目标接受")
        time.sleep(0.2)
        gh.cancel_goal_async()
        rf = gh.get_result_async()
        rclpy.spin_until_future_complete(self.test_cli, rf, timeout_sec=15.0)
        if rf.done():
            res = rf.result()
            code = res.result.exit_code
            out_str = res.result.output_json
            self.log(f"  结果: exit_code={code}, 原始输出前200字符: {out_str[:200]}")
            if code == -7:
                self.assert_true("execution canceled" in out_str, "包含取消信息")
            elif code == 0:
                self.log("  取消时文件已写完，验证文件大小")
                try:
                    data = json.loads(out_str)
                    size = int(data.get("size", -1))
                    self.assert_equal(size, BIG_CONTENT_MB * 1024 * 1024, "文件大小完整")
                except Exception as e:
                    self.assert_true(False, f"解析输出失败: {e}")
            else:
                self.assert_true(False, f"意外退出码: {code}")
        else:
            self.assert_true(False, "未收到结果")
        if os.path.exists(tmp): os.unlink(tmp)

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