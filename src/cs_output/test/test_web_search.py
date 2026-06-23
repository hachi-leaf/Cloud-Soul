#!/usr/bin/env python3
"""
测试脚本：web_search_node 工具节点功能验证
正确遵循 output_mgmt_node 协议：
  input_json = {"name": "web_search", "arguments": <dict>}
运行方式：python3 test_web_search.py [--agent agent_name]
"""
import subprocess, signal, os, time, sys, argparse, json
import rclpy
from rclpy.node import Node
from cs_interfaces.srv import GetToolsInfo
from cs_interfaces.action import ExecuteTool
from rclpy.action import ActionClient

MGMT_NODE = "output_mgmt_node"
TOOL_NODE = "web_search_node"
AGENT_NAME = "agent"

def force_clean():
    for name in [MGMT_NODE, TOOL_NODE]:
        os.system(f"pkill -9 -f {name} 2>/dev/null")

class TestClient(Node):
    def __init__(self):
        super().__init__('test_client', namespace=AGENT_NAME)
        self.tools_cli = self.create_client(GetToolsInfo, f'/{AGENT_NAME}/output/info')
        self.action_cli = ActionClient(self, ExecuteTool, f'/{AGENT_NAME}/output')

    def call_info_service(self):
        if not self.tools_cli.wait_for_service(5.0):
            return None
        req = GetToolsInfo.Request()
        future = self.tools_cli.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=5.0)
        if future.done():
            return future.result().tools_json
        return None

    def send_tool_call(self, tool_name, arguments: dict, timeout_sec=60.0):
        """
        arguments: 字典，直接作为 arguments 对象嵌入 input_json。
        管理节点要求 arguments 是 JSON 对象，不是字符串。
        """
        if not isinstance(arguments, dict):
            raise TypeError("arguments must be a dict")
        input_json = json.dumps({"name": tool_name, "arguments": arguments})
        goal = ExecuteTool.Goal()
        goal.input_json = input_json
        goal.timeout_sec = timeout_sec
        if not self.action_cli.wait_for_server(5.0):
            return None, None
        future = self.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(self, future, timeout_sec=5.0)
        if not future.done():
            return None, None
        goal_handle = future.result()
        if not goal_handle.accepted:
            return None, None
        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(self, result_future, timeout_sec=timeout_sec+5)
        if not result_future.done():
            return None, None
        res = result_future.result().result
        return res.exit_code, res.output_json

# --------- 测试用例 ---------
def test_ip(client):
    code, out = client.send_tool_call("web_search", {"action":"ip_info","query":""})
    assert code == 0, f"ip_info failed: {out}"
    data = json.loads(out)
    print(f"✅ ip_info test passed: IP={data.get('ip')} City={data.get('city')} ISP={data.get('isp')}")

def test_time(client):
    code, out = client.send_tool_call("web_search", {"action":"time","query":"","timezone":"Asia/Shanghai"})
    assert code == 0, f"time failed: {out}"
    data = json.loads(out)
    print(f"✅ time test passed: {data.get('datetime')} ({data.get('timezone')})")

def test_calc(client):
    code, out = client.send_tool_call("web_search", {"action":"calculator","query":"2+3*5"})
    assert code == 0, f"calculator failed: {out}"
    data = json.loads(out)
    print(f"✅ calculator test passed: {data.get('expression')} = {data.get('result')}")

def test_dict(client):
    code, out = client.send_tool_call("web_search", {"action":"dictionary","query":"example"})
    assert code == 0, f"dictionary failed: {out}"
    data = json.loads(out)
    phonetic = data.get("phonetic", "N/A")
    meanings = data.get("meanings", [])
    first_def = meanings[0]["definitions"][0] if meanings and meanings[0].get("definitions") else "N/A"
    print(f"✅ dictionary test passed: word={data.get('word')} phonetic={phonetic} def={first_def}")

def test_translate(client):
    code, out = client.send_tool_call("web_search", {"action":"translate","query":"RDK X5 开发板是地瓜机器人的产品之一。","target_lang":"en"})
    assert code == 0, f"translate failed: {out}"
    data = json.loads(out)
    print(f"✅ translate test passed: {data.get('translated_text')}")

def test_qrcode(client):
    code, out = client.send_tool_call("web_search", {"action":"qrcode","query":"https://example.com"})
    assert code == 0, f"qrcode failed: {out}"
    data = json.loads(out)
    print(f"✅ qrcode test passed: image_url={data.get('image_url')[:60]}...")

def test_weather(client):
    code, out = client.send_tool_call("web_search", {"action":"weather","query":"London"})
    assert code == 0, f"weather failed: {out}"
    data = json.loads(out)
    print(f"✅ weather test passed: {data.get('city')} {data.get('temp_C')}°C {data.get('description')}")

def test_search(client):
    code, out = client.send_tool_call("web_search", {"action":"search","query":"ROS2","max_results":2})
    assert code == 0, f"search failed: {out}"
    data = json.loads(out)
    results = data.get("results", [])
    if results:
        print(f"✅ search test passed: first title='{results[0].get('title')}' snippet='{results[0].get('snippet', '')[:80]}...'")
    else:
        print("✅ search test passed: no results")

def test_news(client):
    code, out = client.send_tool_call("web_search", {"action":"news","query":"technology","max_results":1})
    assert code == 0, f"news failed: {out}"
    data = json.loads(out)
    articles = data.get("articles", [])
    if articles:
        print(f"✅ news test passed: first article: {articles[0].get('title')}")
    else:
        print("✅ news test passed: no articles")

def test_cancel(client):
    # 保持不变，无需打印内容
    max_attempts = 3
    for attempt in range(max_attempts):
        goal = ExecuteTool.Goal()
        goal.input_json = json.dumps({
            "name": "web_search",
            "arguments": {"action":"search","query":"long running query","max_results":100}
        })
        goal.timeout_sec = 60.0
        if not client.action_cli.wait_for_server(5.0):
            print("❌ cancel test: action server not ready")
            return
        send_future = client.action_cli.send_goal_async(goal)
        rclpy.spin_until_future_complete(client, send_future, timeout_sec=5.0)
        if not send_future.done():
            print("❌ cancel test: goal not accepted")
            return
        goal_handle = send_future.result()
        if not goal_handle.accepted:
            print("❌ cancel test: goal rejected")
            return
        time.sleep(0.3)
        cancel_future = goal_handle.cancel_goal_async()
        rclpy.spin_until_future_complete(client, cancel_future, timeout_sec=5.0)
        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(client, result_future, timeout_sec=15.0)
        if not result_future.done():
            print("❌ cancel test: result not received")
            return
        res = result_future.result().result
        code, out = res.exit_code, res.output_json
        if code == -7:
            print("✅ cancel test passed (exit_code=-7)")
            return
        print(f"⚠️  Attempt {attempt+1}: got exit_code {code} instead of -7, retrying...")
        time.sleep(0.5)
    print(f"❌ cancel test failed after {max_attempts} attempts, last code={code}, out={out}")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--agent', default='agent')
    args = parser.parse_args()
    global AGENT_NAME
    AGENT_NAME = args.agent

    force_clean()
    mgmt = subprocess.Popen(
        f'ros2 run cs_output output_mgmt_node --ros-args -p agent_name:={AGENT_NAME}',
        shell=True, start_new_session=True)
    tool = subprocess.Popen(
        f'ros2 run cs_output web_search_node --ros-args -p agent_name:={AGENT_NAME}',
        shell=True, start_new_session=True)
    time.sleep(3)

    try:
        rclpy.init()
        client = TestClient()
        # 等待工具被发现
        for _ in range(10):
            info = client.call_info_service()
            if info and 'web_search' in info:
                break
            time.sleep(1)
        else:
            raise RuntimeError("Tool not discovered")

        print("Running tests...")
        test_ip(client)
        test_calc(client)
        test_translate(client)
        test_qrcode(client)
        test_weather(client)
        test_search(client)
        test_news(client)
        test_cancel(client)
        print("\n🎉 All tests passed!")
    except Exception as e:
        print(f"❌ Test failed: {e}")
    finally:
        rclpy.shutdown()
        for p in [tool, mgmt]:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGINT)
                p.wait(3)
            except:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
        force_clean()

if __name__ == '__main__':
    main()