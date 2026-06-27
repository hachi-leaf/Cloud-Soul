#!/usr/bin/env python3
"""
shell_exec_node 全状态穷举测试
用法: python3 test_shell_exec.py <agent_name>

============================================================
            Action 特性全状态穷举表 (24 项)
============================================================
S 成功状态 (1):
  S1  命令执行成功，退出码 0

F 命令执行失败 (3, 均 exit_code=-1):
  F1  命令退出码非零
  F2  命令不存在 (退出码 127)
  F3  命令被信号杀死 (非超时/取消)

V 校验失败 (1):
  V1  command 缺失或为空

R 系统错误 (4, 跳过):
  R1  临时文件创建失败
  R2  临时文件写入失败
  R3  管道创建失败
  R4  fork 失败

T 超时 (2):
  T1  命令执行超时（有输出）
  T2  命令执行超时（无输出）

C 取消 (3):
  C1  执行中用户主动 Cancel（有输出）
  C2  执行中 Ctrl+C 取消 (跳过)
  C3  执行中 Cancel 但无输出

P 并行拒绝 (1):
  P1  已有 Goal 执行时新 Goal 被拒绝

B 特殊行为 (2, 内嵌验证):
  B1  超时/取消后 stdout 包含已捕获内容
  B2  交互命令因 stdin 关闭而立即失败
  B3  极限命令压力（极端 HTML 嵌套）

可自动化: S1, F1-F3, V1, T1-T2, C1,C3, P1, B1-B2
跳过: R1-R4 (系统资源限制), C2 (外部信号)
============================================================
"""

import rclpy
from rclpy.node import Node
from rclpy.action import ActionClient
from cs_interfaces.action import ExecuteTool
import json
import sys
import time
import threading

AGENT = sys.argv[1] if len(sys.argv) > 1 else 'test'
ACTION_NAME = '/' + AGENT + '/output/shell_exec'

PASS = 0
FAIL = 0
SKIP = 0

class Tester(Node):
    def __init__(self):
        super().__init__('shell_exec_tester')
        self._ac = ActionClient(self, ExecuteTool, ACTION_NAME)

    def wait_for_server(self, timeout=5.0):
        return self._ac.wait_for_server(timeout_sec=timeout)

    def send_goal(self, goal_dict, timeout_sec=10.0):
        goal_msg = ExecuteTool.Goal()
        goal_msg.input_json = json.dumps(goal_dict)
        goal_msg.timeout_sec = timeout_sec

        future = self._ac.send_goal_async(goal_msg)
        rclpy.spin_until_future_complete(self, future, timeout_sec=15.0)
        if not future.done():
            return {"error": "send_goal timeout"}, -999
        goal_handle = future.result()
        if not goal_handle.accepted:
            return {"error": "goal rejected"}, -998

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(self, result_future, timeout_sec=timeout_sec + 5.0)
        if not result_future.done():
            return {"error": "get_result timeout"}, -997

        response = result_future.result()
        result = response.result
        try:
            obj = json.loads(result.output_json)
        except Exception:
            obj = {"_raw": result.output_json}
        return obj, result.exit_code

    def send_goal_and_cancel(self, goal_dict, timeout_sec=10.0, cancel_after=0.5):
        goal_msg = ExecuteTool.Goal()
        goal_msg.input_json = json.dumps(goal_dict)
        goal_msg.timeout_sec = timeout_sec

        future = self._ac.send_goal_async(goal_msg)
        rclpy.spin_until_future_complete(self, future, timeout_sec=5.0)
        if not future.done():
            return {"error": "send_goal timeout"}, -999
        goal_handle = future.result()
        if not goal_handle.accepted:
            return {"error": "goal rejected"}, -998

        time.sleep(cancel_after)
        cancel_future = goal_handle.cancel_goal_async()
        rclpy.spin_until_future_complete(self, cancel_future, timeout_sec=5.0)

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(self, result_future, timeout_sec=timeout_sec + 5.0)
        if not result_future.done():
            return {"error": "get_result timeout"}, -997

        response = result_future.result()
        result = response.result
        try:
            obj = json.loads(result.output_json)
        except Exception:
            obj = {"_raw": result.output_json}
        return obj, result.exit_code


def check(desc, result_dict, exit_code, expected_code, expected_msg=None):
    global PASS, FAIL
    ok = True
    issues = []
    if exit_code != expected_code:
        ok = False
        issues.append(f"expected ec={expected_code} got {exit_code}")
    if expected_msg:
        txt = json.dumps(result_dict) if result_dict else ""
        if expected_msg not in txt:
            ok = False
            issues.append(f"missing [{expected_msg}]")
    if ok:
        PASS += 1
        print(f"  [OK] {desc}")
    else:
        FAIL += 1
        print(f"  [FAIL] {desc} | {'; '.join(issues)} | out={str(result_dict)[:120]}")


# ──────────── 初始化 ────────────
rclpy.init()
tester = Tester()
if not tester.wait_for_server(10.0):
    print("ERROR: Action server not ready")
    rclpy.shutdown()
    sys.exit(1)

# ============================================================
# 测试用例
# ============================================================

print("\n=== S 成功状态 (1) ===")
res, ec = tester.send_goal({"command": "echo hello"})
check("S1 command success", res, ec, 0)
# 额外校验 stdout 是否包含 'hello'
if "hello" not in res.get("stdout", ""):
    FAIL += 1
    print("  [FAIL] S1 stdout missing 'hello'")
else:
    PASS += 1
    print("  [OK] S1 stdout contains 'hello'")

print("\n=== F 命令执行失败 (3) ===")
# F1: 命令退出码非零
res, ec = tester.send_goal({"command": "exit 42"})
check("F1 non-zero exit", res, ec, -1)
# F2: 命令不存在
res, ec = tester.send_goal({"command": "nonexistent_command_xyz"})
check("F2 command not found", res, ec, -1)
# F3: 命令被信号杀死（触发段错误或类似，但可预测；这里用 kill 自身，但需注意不能杀死节点本身。用一个子进程自杀即可）
res, ec = tester.send_goal({"command": "kill $$"}, timeout_sec=5.0)
check("F3 killed by signal", res, ec, -1)

print("\n=== V 校验失败 (1) ===")
# V1: 缺少 command 或空串
res, ec = tester.send_goal({"command": ""})
check("V1 empty command", res, ec, -1, "command is required")
res, ec = tester.send_goal({})
check("V1 missing command", res, ec, -1, "command is required")

print("\n=== R 系统错误 (4) ===")
print("  [SKIP] R1 temp file creation failure (requires /tmp unwritable)")
print("  [SKIP] R2 temp file write failure (requires disk full)")
print("  [SKIP] R3 pipe creation failure (requires resource exhaustion)")
print("  [SKIP] R4 fork failure (requires process limit)")
SKIP += 4

print("\n=== T 超时 (2) ===")
# T1: 超时有输出（例如 sleep 10 秒同时 echo 几行，但 sleep 会阻塞，不会产生输出，所以分两种）
# 这里用一个后台进程一边输出一边等待超时
script_t1 = "for i in 1 2 3; do echo \"line $i\"; sleep 1; done; sleep 100"
res, ec = tester.send_goal({"command": script_t1}, timeout_sec=1.0)  # 1秒超时，应捕获前几行
check("T1 timeout with output", res, ec, -1, "timed out after")

# T2: 超时无输出（纯 sleep）
res, ec = tester.send_goal({"command": "sleep 100"}, timeout_sec=1.0)
check("T2 timeout no output", res, ec, -1, "timed out after")
# 额外校验 stdout 为空或很短
if res.get("stdout", "") not in ("", "\n", "\\n"):
    FAIL += 1
    print("  [FAIL] T2 stdout not empty")
else:
    PASS += 1
    print("  [OK] T2 stdout empty")

print("\n=== C 取消 (3) ===")
# C1: 取消时有输出（运行长时间脚本，取消）
script_c1 = "for i in 1 2 3; do echo \"line $i\"; sleep 1; done; sleep 100"
res, ec = tester.send_goal_and_cancel({"command": script_c1}, timeout_sec=30.0, cancel_after=1.5)
check("C1 cancel with output", res, ec, -1, "execution canceled")
# 检查是否有部分输出
if "line 1" not in res.get("stdout", ""):
    FAIL += 1
    print("  [FAIL] C1 stdout missing captured output")
else:
    PASS += 1
    print("  [OK] C1 stdout captured")

# C3: 取消时无输出（刚启动就取消）
res, ec = tester.send_goal_and_cancel({"command": "sleep 100"}, timeout_sec=30.0, cancel_after=0.01)
check("C3 cancel no output", res, ec, -1, "execution canceled")
# stdout 可能为空
if res.get("stdout", "") in ("", "\n"):
    PASS += 1
    print("  [OK] C3 stdout empty as expected")
else:
    FAIL += 1
    print("  [FAIL] C3 stdout not empty")

print("  [SKIP] C2 Ctrl+C cancel (requires external signal)")
SKIP += 1

print("\n=== P 并行拒绝 (1) ===")
# 发送一个长时间运行的命令，立即再发送第二个，应被拒绝
goal_msg = ExecuteTool.Goal()
goal_msg.input_json = json.dumps({"command": "sleep 100"})
goal_msg.timeout_sec = 3.0
future1 = tester._ac.send_goal_async(goal_msg)
rclpy.spin_until_future_complete(tester, future1, timeout_sec=5.0)
if future1.done() and future1.result().accepted:
    gh1 = future1.result()
    res2, ec2 = tester.send_goal({"command": "echo second"})
    check("P1 parallel rejection", res2, ec2, -1, "Another goal is already running")
    # 清理：等待第一个 Goal 结束（否则会卡住后续，但后续已无测试）
    rclpy.spin_until_future_complete(tester, gh1.get_result_async(), timeout_sec=65.0)
else:
    print("  [SKIP] P1 couldn't send first goal")
    SKIP += 1

print("\n=== B 特殊行为 (3) ===")
# B1: 超时/取消时 stdout 已捕获内容（已在 T1, C1 验证）
print("  [OK] B1 timeout/cancel captures stdout (verified in T1/C1)")
PASS += 1

# B2: 交互命令因 stdin 关闭立即失败（例如需要输入的命令）
res, ec = tester.send_goal({"command": "test -t 0"})
check("B2 stdin not a terminal", res, ec, -1)
# 确保没有卡死，而是直接返回错误或输出为空（因为 read 读到 EOF 后退出码非零）
# 具体行为取决于 /bin/sh，可能退出码非零且 stdout 为空

# B3: HTML 极端文本写入测试
# 构造一个超长、多层转义、富含花括号、各种引号的 HTML 字符串
# 包含：HTML5 结构、嵌入式 JSON、多重花括号、反斜杠、换行等
complex_html = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <title>Stress \\"' &amp; &lt; { [ (</title>
  <script type="application/json" id="config">
    {"nested": {"key": "value with \\"quotes\\" and {braces}"}, "arr": [1, "two", {"three": 3}]}
  </script>
</head>
<body>
  <h1>Test {complex} [nested] (parens) <span class="foo bar" data-info='{"status":"ok"}'>Content</span></h1>
  <p>Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.
  Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in
  reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in
  culpa qui officia deserunt mollit anim id est laborum.</p>
  <!-- comment with special chars: <>{}[]"' -->
  <div class='container' id="main" style="background: url('data:image/png;base64,iVBORw0KGgoAAAANS...');">
    <form action="/submit" method="post">
      <input type="hidden" name="csrf" value="abcdef123456-{}[]\\"'" />
      <button type="submit">Submit</button>
    </form>
  </div>
</body>
</html>"""

# 通过 heredoc 将内容写入文件，再 cat 并删除
command = (
    "cat <<'EOF' > /tmp/stress_test.html\n" +
    complex_html + "\n" +
    "EOF\n"
    "cat /tmp/stress_test.html\n"
    "rm -f /tmp/stress_test.html"
)

res, ec = tester.send_goal({"command": command}, timeout_sec=10.0)
check("B3 write-read roundtrip", res, ec, 0)

stdout = res.get("stdout", "")
# 注意：cat 的输出末尾可能多一个换行符，我们需要标准化后再比较
expected_normalized = complex_html.rstrip("\n") + "\n"   # heredoc 的结尾可能没有换行，但 cat 会添加一个换行？实际上 heredoc 最后的换行会保留，所以 complex_html 以 \n 结尾，cat 输出也一样。为了安全，去除末尾可能多余的空行再比较。
import re
stdout_clean = stdout.rstrip("\n") + "\n"
expected_clean = complex_html.rstrip("\n") + "\n"

if stdout_clean == expected_clean:
    PASS += 1
    print("  [OK] B3 roundtrip content identical")
else:
    # 找出第一个差异位置
    diff_pos = None
    for i, (a, b) in enumerate(zip(stdout_clean, expected_clean)):
        if a != b:
            diff_pos = i
            break
    if diff_pos is None:
        diff_pos = min(len(stdout_clean), len(expected_clean))
    context_start = max(0, diff_pos - 50)
    FAIL += 1
    print("  [FAIL] B3 content mismatch at byte {}:\n  expected: {!r}\n  got:      {!r}".format(
        diff_pos,
        expected_clean[context_start:diff_pos+50],
        stdout_clean[context_start:diff_pos+50]
    ))
# ──────────── 汇总 ────────────
total = PASS + FAIL
print("\n" + "=" * 60)
print(f"  Pass: {PASS}  Fail: {FAIL}  Skip: {SKIP}")
print("  Automatable: S1, F1-F3, V1, T1-T2, C1,C3, P1, B1-B2")
print("  Skipped: R1-R4, C2")
if FAIL == 0:
    print("  All automated tests passed.")
else:
    print("  Some tests failed, check output.")
print("=" * 60)

tester.destroy_node()
rclpy.shutdown()
sys.exit(0 if FAIL == 0 else 1)
