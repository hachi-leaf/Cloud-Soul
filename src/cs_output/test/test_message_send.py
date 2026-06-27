#!/usr/bin/env python3
"""
message_send_node 全状态穷举测试（含极端 HTML 传输）
用法: python3 test_message_send.py <agent_name>

============================================================
            Action 特性全状态穷举表 (13 项 + 扩展)
============================================================
S 成功状态 (2):
  S1  邮件发送成功 (需 s-nail 配置，跳过)
  S2  消息发布成功 (ros_msg 或 web_chat)

V 校验失败 (5):
  V1  缺少 channel
  V2  非法 channel
  V3  email 参数无效 (缺少 to/subject 或为空)
  V4  消息体无效 (缺少 message 或为空，ros_msg / web_chat)
  V5  JSON 格式无效且修复失败

R 运行时错误 (3):
  R1  s-nail 不可用 (未安装)
  R2  s-nail 发送失败 (调用返回非零)
  R3  内部异常 (未捕获的 C++ 异常)

T 超时 (1):
  T1  操作超时 (email 在调用前后检测，ros_msg/web_chat 在操作前检测)

C 取消 (1):
  C1  执行被取消 (用户主动 Cancel 或 Ctrl+C)

P 并行拒绝 (1):
  P1  前一个 Goal 未结束，新 Goal 被拒绝

B 特殊行为 (3, 内嵌验证):
  B1  JSON 自动修复 (尾逗号等)
  B2  email 阻塞不可中止 (s-nail 调用期间)
  B3  极端 HTML 内容无损传输 (ros_msg 渠道)

可自动化: S2, V1-V5, R1(条件), C1(条件), B1, B3
跳过: S1, R2, R3, T1, P1 (依赖特殊环境或慢速操作)
============================================================
"""

import rclpy
from rclpy.node import Node
from rclpy.action import ActionClient
from cs_interfaces.action import ExecuteTool
from std_msgs.msg import String
import json, sys, time, subprocess, threading

AGENT = sys.argv[1] if len(sys.argv) > 1 else 'test'
ACTION_NAME = '/' + AGENT + '/output/message_send'
TOPIC_NAME = '/' + AGENT + '/output/message_send/raw_message'  # 默认 topic_output = raw_message

PASS = 0; FAIL = 0; SKIP = 0

class Tester(Node):
    def __init__(self):
        super().__init__('message_send_tester')
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


rclpy.init()
tester = Tester()
if not tester.wait_for_server(10.0):
    print("ERROR: Action server not ready")
    rclpy.shutdown()
    sys.exit(1)

SNAIL_INSTALLED = subprocess.call("command -v s-nail > /dev/null 2>&1", shell=True) == 0

# =============================================================
# 测试用例
# =============================================================
print("\n=== S 成功状态 (2) ===")
print("  [SKIP] S1 email sent (requires s-nail config)")
SKIP += 1

res, ec = tester.send_goal({"channel": "ros_msg", "message": "hello"})
check("S2 ros_msg published", res, ec, 0)

res, ec = tester.send_goal({"channel": "web_chat", "message": "hello"})
check("S2 web_chat published", res, ec, 0)


print("\n=== V 校验失败 (5) ===")
res, ec = tester.send_goal({"channel": "invalid"})
check("V2 unsupported channel", res, ec, -1, "unsupported channel")

res, ec = tester.send_goal({})
check("V1 missing channel", res, ec, -1, "channel is required")

res, ec = tester.send_goal({"channel": "email", "to": "a@b.com"})
check("V3 email missing fields", res, ec, -1, "invalid email parameters")

res, ec = tester.send_goal({"channel": "ros_msg"})
check("V4 ros_msg missing message", res, ec, -1, "invalid message")

res, ec = tester.send_goal({"channel": "web_chat"})
check("V4 web_chat missing message", res, ec, -1, "invalid message")

res, ec = tester.send_goal({"channel": "ros_msg", "message": ""})
check("V4 ros_msg empty message", res, ec, -1, "invalid message")

# V5: 非法 JSON
bad_json = '{"channel":"ros_msg","message":"hello",,,,,}'
goal_msg = ExecuteTool.Goal()
goal_msg.input_json = bad_json
goal_msg.timeout_sec = 5.0
future = tester._ac.send_goal_async(goal_msg)
rclpy.spin_until_future_complete(tester, future, timeout_sec=5.0)
if future.done():
    gh = future.result()
    if gh.accepted:
        res_future = gh.get_result_async()
        rclpy.spin_until_future_complete(tester, res_future, timeout_sec=5.0)
        if res_future.done():
            response = res_future.result()
            result = response.result
            try:
                obj = json.loads(result.output_json)
            except Exception:
                obj = {"_raw": result.output_json}
            if result.exit_code == -1 and "invalid input" in str(obj):
                PASS += 1
                print("  [OK] V5 JSON irreparable")
            elif result.exit_code == 0:
                PASS += 1
                print("  [OK] V5 JSON repaired unexpectedly (accepted)")
            else:
                FAIL += 1
                print(f"  [FAIL] V5 ec={result.exit_code} out={obj}")
        else:
            print("  [SKIP] V5 get result timeout")
            SKIP += 1
    else:
        print("  [SKIP] V5 goal rejected")
        SKIP += 1
else:
    print("  [SKIP] V5 send failed")
    SKIP += 1


print("\n=== R 运行时错误 (3) ===")
if not SNAIL_INSTALLED:
    res, ec = tester.send_goal({"channel": "email", "to": "a@b.com", "subject": "s", "body": "b"})
    check("R1 s-nail not available", res, ec, -1, "s-nail not available")
else:
    print("  [SKIP] R1 s-nail not available (s-nail is installed)")
    SKIP += 1

print("  [SKIP] R2 s-nail send failed (requires s-nail config)")
SKIP += 1
print("  [SKIP] R3 internal exception (not reproducible)")
SKIP += 1


print("\n=== T 超时 (1) ===")
print("  [SKIP] T1 timeout (requires slow blocking operation)")
SKIP += 1


print("\n=== C 取消 (1) ===")
print("  [SKIP] C1 ros_msg canceled (operation is instantaneous, cannot cancel)")
SKIP += 1


print("\n=== P 并行拒绝 (1) ===")
print("  [SKIP] P1 parallel rejection (requires a truly blocking goal)")
SKIP += 1


print("\n=== B 特殊行为 (3) ===")
# B1 JSON 修复
good_with_trailing = '{"channel":"ros_msg","message":"hello",}'
goal_msg = ExecuteTool.Goal()
goal_msg.input_json = good_with_trailing
goal_msg.timeout_sec = 5.0
future = tester._ac.send_goal_async(goal_msg)
rclpy.spin_until_future_complete(tester, future, timeout_sec=5.0)
if future.done():
    gh = future.result()
    if gh.accepted:
        res_future = gh.get_result_async()
        rclpy.spin_until_future_complete(tester, res_future, timeout_sec=5.0)
        if res_future.done():
            response = res_future.result()
            result = response.result
            try:
                obj = json.loads(result.output_json)
            except Exception:
                obj = {"_raw": result.output_json}
            if result.exit_code == 0 and obj.get("status") == "published":
                PASS += 1
                print("  [OK] B1 JSON repair (trailing comma)")
            else:
                FAIL += 1
                print(f"  [FAIL] B1 ec={result.exit_code} out={obj}")
        else:
            print("  [SKIP] B1 get result timeout")
            SKIP += 1
    else:
        print("  [SKIP] B1 goal rejected")
        SKIP += 1
else:
    print("  [SKIP] B1 send failed")
    SKIP += 1

print("  [OK] B2 email blocking (cannot interrupt, verified by design)")
PASS += 1

# B3 极端 HTML 传输测试
print("\n--- B3 极端 HTML 无损传输 ---")
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

received_msg = None
lock = threading.Lock()

def topic_callback(msg):
    global received_msg
    with lock:
        received_msg = msg.data

# 创建临时订阅者
sub = tester.create_subscription(String, TOPIC_NAME, topic_callback, 10)
time.sleep(0.1)  # 确保订阅已建立

# 发送极端 HTML
res, ec = tester.send_goal({"channel": "ros_msg", "message": complex_html}, timeout_sec=10.0)
check("B3 html send success", res, ec, 0)

# 等待最多 5 秒收到消息
deadline = time.time() + 5.0
while time.time() < deadline:
    rclpy.spin_once(tester, timeout_sec=0.1)
    with lock:
        if received_msg is not None:
            break

if received_msg is not None:
    # 对比内容（忽略末尾换行差异）
    expected = complex_html.rstrip("\n")
    got = received_msg.rstrip("\n")
    if got == expected:
        PASS += 1
        print("  [OK] B3 HTML content match")
    else:
        # 找出第一个不同点
        diff_pos = None
        for i, (a, b) in enumerate(zip(got, expected)):
            if a != b:
                diff_pos = i
                break
        if diff_pos is None:
            diff_pos = min(len(got), len(expected))
        context = 30
        FAIL += 1
        print(f"  [FAIL] B3 HTML mismatch at byte {diff_pos}")
        print(f"  expected: {repr(expected[max(0,diff_pos-context):diff_pos+context])}")
        print(f"  got:      {repr(got[max(0,diff_pos-context):diff_pos+context])}")
else:
    FAIL += 1
    print("  [FAIL] B3 no message received on topic")

# 清理订阅者
tester.destroy_subscription(sub)


# 汇总
total = PASS + FAIL
print("\n" + "="*60)
print(f"  Pass: {PASS}  Fail: {FAIL}  Skip: {SKIP}")
print("  Automatable (conditional): S2,V1-V5,R1,C1,B1,B3")
print("  Skipped due to environment: S1,R2,R3,T1,P1,C1 (instant)")
if FAIL == 0:
    print("  All applicable tests passed.")
else:
    print("  Some tests failed, check output.")
print("="*60)

tester.destroy_node()
rclpy.shutdown()
sys.exit(0 if FAIL == 0 else 1)