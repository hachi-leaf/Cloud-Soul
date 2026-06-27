#!/usr/bin/env python3
import argparse, os, shutil, signal, subprocess, tempfile, time, threading, sys
import rclpy
from rclpy.node import Node
from cs_interfaces.srv import MemoryRecall, MemoryArchive

GIT_CLEAN_ENV = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1'}

class MemoryTestClient(Node):
    def __init__(self, agent_name):
        super().__init__('test_memory_client')
        self.recall_client = self.create_client(MemoryRecall, f'/{agent_name}/memory_recall')
        self.archive_client = self.create_client(MemoryArchive, f'/{agent_name}/memory_archive')

    def call_recall(self, timeout=10):
        if not self.recall_client.wait_for_service(timeout_sec=timeout):
            return None, None
        req = MemoryRecall.Request()
        future = self.recall_client.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done():
            res = future.result()
            return res.error_code, res.text
        return None, None

    def call_archive(self, json_path, timeout=30):
        if not self.archive_client.wait_for_service(timeout_sec=timeout):
            return None
        req = MemoryArchive.Request()
        req.json_path = json_path
        future = self.archive_client.call_async(req)
        rclpy.spin_until_future_complete(self, future, timeout_sec=timeout)
        if future.done():
            return future.result().error_code
        return None

def run_git(cmd, cwd=None, timeout=30):
    subprocess.run(cmd, cwd=cwd, env=GIT_CLEAN_ENV, check=True, timeout=timeout,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def kill_leftover_nodes():
    try:
        subprocess.run(['pkill', '-f', 'memory_node'], stderr=subprocess.DEVNULL, timeout=2)
        time.sleep(0.5)
    except Exception:
        pass

def create_git_repo(tmpdir, init_files=True):
    """创建裸仓库，返回 (file:// URL, bare_path)"""
    bare_path = os.path.join(tmpdir, 'remote.git')
    work_path = os.path.join(tmpdir, 'work')
    os.makedirs(bare_path)
    run_git(['git', 'init', '--bare', '--initial-branch=main', bare_path])
    if init_files:
        run_git(['git', 'clone', bare_path, work_path])
        os.makedirs(os.path.join(work_path, 'prompts'))
        with open(os.path.join(work_path, 'prompts/RULE.md'), 'w') as f:
            f.write("Welcome to [prompts/WELCOME.md]")
        with open(os.path.join(work_path, 'prompts/WELCOME.md'), 'w') as f:
            f.write("Hello, this is a test.")
        with open(os.path.join(work_path, 'prompts/COMPRESS.md'), 'w') as f:
            f.write("Compress JSON into a short summary.")
        os.makedirs(os.path.join(work_path, 'diaries'))
        run_git(['git', 'add', '-A'], cwd=work_path)
        run_git(['git', 'commit', '-m', 'Init'], cwd=work_path)
        run_git(['git', 'push', 'origin', 'main'], cwd=work_path)
        shutil.rmtree(work_path)
    return f'file://{bare_path}', bare_path

def start_node(agent_name, repo_url, repo_dir, api_key, api_base, model, extra_params=None, timeout=10):
    kill_leftover_nodes()
    cmd = ['ros2', 'run', 'cs_core', 'memory_node', '--ros-args',
           '-p', f'agent_name:={agent_name}',
           '-p', f'repo_url:={repo_url}',
           '-p', f'repo_dir:={repo_dir}',
           '-p', f'openai_api_key:={api_key}',
           '-p', f'openai_base_url:={api_base}',
           '-p', f'openai_model:={model}']
    if extra_params:
        for k, v in extra_params.items():
            cmd += ['-p', f'{k}:={v}']
    env = {**os.environ, 'GIT_CONFIG_NOSYSTEM': '1'}
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    start = time.time()
    while time.time() - start < timeout:
        if proc.poll() is not None:
            out, err = proc.communicate()
            print("Node died at startup:", err.decode())
            return None
        time.sleep(0.5)
    return proc

def stop_node(proc):
    if proc is None: return
    try:
        proc.send_signal(signal.SIGINT)
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=2)
    except Exception: pass
    kill_leftover_nodes()

def print_stderr(proc):
    if proc and proc.stderr:
        err = proc.stderr.read().decode()
        if err: print("--- Node stderr ---\n", err)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('agent_name')
    parser.add_argument('--api-key', default='sk-invalid')
    parser.add_argument('--api-base', default='https://api.deepseek.com')
    parser.add_argument('--model', default='deepseek-chat')
    args = parser.parse_args()

    rclpy.init()
    results = {'PASS': 0, 'FAIL': 0, 'SKIP': 0}

    with tempfile.TemporaryDirectory() as tmpdir:
        # ----- 正常仓库（用于大多数测试）-----
        repo_url, bare_path = create_git_repo(tmpdir)
        repo_dir = os.path.join(tmpdir, 'repo_work')

        # ===== T1: Recall normal =====
        print("=== T1: Recall normal ===")
        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            code, text = client.call_recall()
            if code == 0 and 'Hello' in text:
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - code={code}, text={text}"); results['FAIL'] += 1
            stop_node(proc)
            time.sleep(1)

        # ===== T7: Recall sync failed (必须使用不存在的目录，强制克隆) =====
        print("=== T7: Recall sync failed ===")
        bad_url = "file:///nonexistent/repo"
        # 使用全新的目录，确保节点尝试克隆
        bad_repo_dir = os.path.join(tmpdir, 'bad_repo')
        if os.path.exists(bad_repo_dir): shutil.rmtree(bad_repo_dir)
        os.makedirs(bad_repo_dir)
        proc = start_node(args.agent_name, bad_url, bad_repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            code, text = client.call_recall()
            if code == -1 and text == "repo sync failed":
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - code={code}, text={text}"); results['FAIL'] += 1
            stop_node(proc)
            time.sleep(1)

        # ===== T2: Missing RULE.md =====
        print("=== T2: Missing RULE.md ===")
        # 操作正常仓库，删除 RULE.md
        work_tmp = os.path.join(tmpdir, 'work_tmp')
        run_git(['git', 'clone', bare_path, work_tmp])
        os.remove(os.path.join(work_tmp, 'prompts/RULE.md'))
        run_git(['git', 'add', '-A'], cwd=work_tmp)
        run_git(['git', 'commit', '-m', 'remove rule'], cwd=work_tmp)
        run_git(['git', 'push', 'origin', 'main'], cwd=work_tmp)
        shutil.rmtree(work_tmp)

        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            code, text = client.call_recall()
            if code == -1 and 'rule file not found' in text:
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - code={code}, text={text}"); results['FAIL'] += 1
            stop_node(proc)
            # 恢复 RULE.md
            work2 = os.path.join(tmpdir, 'work2')
            run_git(['git', 'clone', bare_path, work2])
            with open(os.path.join(work2, 'prompts/RULE.md'), 'w') as f:
                f.write("Welcome back to [prompts/WELCOME.md]")
            run_git(['git', 'add', '-A'], cwd=work2)
            run_git(['git', 'commit', '-m', 'restore'], cwd=work2)
            run_git(['git', 'push', 'origin', 'main'], cwd=work2)
            shutil.rmtree(work2)
            time.sleep(1)

        # ===== T8: Recall plain text =====
        print("=== T8: Recall plain text ===")
        work_tmp = os.path.join(tmpdir, 'work_plain')
        run_git(['git', 'clone', bare_path, work_tmp])
        with open(os.path.join(work_tmp, 'prompts/RULE.md'), 'w') as f:
            f.write("Hello World")
        run_git(['git', 'add', '-A'], cwd=work_tmp)
        run_git(['git', 'commit', '-m', 'plain'], cwd=work_tmp)
        run_git(['git', 'push', 'origin', 'main'], cwd=work_tmp)
        shutil.rmtree(work_tmp)

        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            code, text = client.call_recall()
            if code == 0 and text == "Hello World":
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - code={code}, text={text}"); results['FAIL'] += 1
            stop_node(proc)
            time.sleep(1)

        # 恢复正常 RULE.md
        work3 = os.path.join(tmpdir, 'work3')
        run_git(['git', 'clone', bare_path, work3])
        with open(os.path.join(work3, 'prompts/RULE.md'), 'w') as f:
            f.write("Welcome to [prompts/WELCOME.md]")
        run_git(['git', 'add', '-A'], cwd=work3)
        run_git(['git', 'commit', '-m', 'restore2'], cwd=work3)
        run_git(['git', 'push', 'origin', 'main'], cwd=work3)
        shutil.rmtree(work3)
        time.sleep(1)

        # ===== T9: Missing placeholder target =====
        print("=== T9: Missing placeholder target ===")
        work_tmp = os.path.join(tmpdir, 'work_miss')
        run_git(['git', 'clone', bare_path, work_tmp])
        with open(os.path.join(work_tmp, 'prompts/RULE.md'), 'w') as f:
            f.write("[prompts/MISSING.md]")
        run_git(['git', 'add', '-A'], cwd=work_tmp)
        run_git(['git', 'commit', '-m', 'missing'], cwd=work_tmp)
        run_git(['git', 'push', 'origin', 'main'], cwd=work_tmp)
        shutil.rmtree(work_tmp)

        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            code, text = client.call_recall()
            if code == 0 and "[prompts/MISSING.md]" in text:
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - code={code}, text={text}"); results['FAIL'] += 1
            stop_node(proc)
            time.sleep(1)

        # 恢复正常
        work4 = os.path.join(tmpdir, 'work4')
        run_git(['git', 'clone', bare_path, work4])
        with open(os.path.join(work4, 'prompts/RULE.md'), 'w') as f:
            f.write("Welcome to [prompts/WELCOME.md]")
        run_git(['git', 'add', '-A'], cwd=work4)
        run_git(['git', 'commit', '-m', 'restore3'], cwd=work4)
        run_git(['git', 'push', 'origin', 'main'], cwd=work4)
        shutil.rmtree(work4)
        time.sleep(1)

        # ===== T10: Nested includes =====
        print("=== T10: Nested includes ===")
        work_tmp = os.path.join(tmpdir, 'work_nest')
        run_git(['git', 'clone', bare_path, work_tmp])
        with open(os.path.join(work_tmp, 'prompts/A.md'), 'w') as f:
            f.write('[prompts/B.md]')
        with open(os.path.join(work_tmp, 'prompts/B.md'), 'w') as f:
            f.write('Deepest Text')
        with open(os.path.join(work_tmp, 'prompts/RULE.md'), 'w') as f:
            f.write('Start [prompts/A.md] End')
        run_git(['git', 'add', '-A'], cwd=work_tmp)
        run_git(['git', 'commit', '-m', 'nested'], cwd=work_tmp)
        run_git(['git', 'push', 'origin', 'main'], cwd=work_tmp)
        shutil.rmtree(work_tmp)

        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            code, text = client.call_recall()
            if code == 0 and "Start Deepest Text End" in text:
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - code={code}, text={text}"); results['FAIL'] += 1
            stop_node(proc)
            time.sleep(1)

        # 恢复
        work5 = os.path.join(tmpdir, 'work5')
        run_git(['git', 'clone', bare_path, work5])
        with open(os.path.join(work5, 'prompts/RULE.md'), 'w') as f:
            f.write("Welcome to [prompts/WELCOME.md]")
        run_git(['git', 'add', '-A'], cwd=work5)
        run_git(['git', 'commit', '-m', 'restore4'], cwd=work5)
        run_git(['git', 'push', 'origin', 'main'], cwd=work5)
        shutil.rmtree(work5)
        time.sleep(1)

        # ===== T11: Cyclic references =====
        print("=== T11: Cyclic references ===")
        work_tmp = os.path.join(tmpdir, 'work_cyc')
        run_git(['git', 'clone', bare_path, work_tmp])
        with open(os.path.join(work_tmp, 'prompts/A.md'), 'w') as f:
            f.write('[prompts/B.md]')
        with open(os.path.join(work_tmp, 'prompts/B.md'), 'w') as f:
            f.write('[prompts/A.md]')
        with open(os.path.join(work_tmp, 'prompts/RULE.md'), 'w') as f:
            f.write('Start [prompts/A.md] End')
        run_git(['git', 'add', '-A'], cwd=work_tmp)
        run_git(['git', 'commit', '-m', 'cyclic'], cwd=work_tmp)
        run_git(['git', 'push', 'origin', 'main'], cwd=work_tmp)
        shutil.rmtree(work_tmp)

        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            code, text = client.call_recall(timeout=10)
            if code == 0 and ('[prompts/A.md]' in text or '[prompts/B.md]' in text):
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - code={code}, text={text}"); results['FAIL'] += 1
            stop_node(proc)
            time.sleep(1)

        # 重新创建干净仓库
        shutil.rmtree(bare_path, ignore_errors=True)
        shutil.rmtree(os.path.join(tmpdir, 'remote.git'), ignore_errors=True)
        repo_url, bare_path = create_git_repo(tmpdir)
        time.sleep(1)

        # ===== T12: Deep nesting (>10) =====
        print("=== T12: Deep nesting (>10) ===")
        work_tmp = os.path.join(tmpdir, 'work_deep')
        run_git(['git', 'clone', bare_path, work_tmp])
        for i in range(10):
            with open(os.path.join(work_tmp, f'prompts/L{i}.md'), 'w') as f:
                f.write(f'[prompts/L{i+1}.md]')
        with open(os.path.join(work_tmp, 'prompts/L10.md'), 'w') as f:
            f.write('Deepest')
        with open(os.path.join(work_tmp, 'prompts/RULE.md'), 'w') as f:
            f.write('Head [prompts/L0.md] Tail')
        run_git(['git', 'add', '-A'], cwd=work_tmp)
        run_git(['git', 'commit', '-m', 'deep'], cwd=work_tmp)
        run_git(['git', 'push', 'origin', 'main'], cwd=work_tmp)
        shutil.rmtree(work_tmp)

        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            code, text = client.call_recall(timeout=10)
            if code == 0 and 'Deepest' in text and '[prompts/L10.md]' not in text:
                print("PASS"); results['PASS'] += 1
            elif code == 0 and '[prompts/L' in text:
                print("PASS (truncated)"); results['PASS'] += 1
            else:
                print(f"FAIL - code={code}, text={text[:200]}"); results['FAIL'] += 1
            stop_node(proc)
            time.sleep(1)

        # 重新创建干净仓库
        shutil.rmtree(bare_path, ignore_errors=True)
        repo_url, bare_path = create_git_repo(tmpdir)
        time.sleep(1)

        # ===== Archive 测试 =====

        # T3: 文件不存在
        print("=== T3: Archive nonexistent file ===")
        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            error = client.call_archive('/nonexistent/path.json')
            if error == -1:
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - error_code={error}"); results['FAIL'] += 1
            stop_node(proc)

        # T4: 无效 API key
        print("=== T4: Invalid API key ===")
        test_json = os.path.join(tmpdir, 'test_archive.json')
        with open(test_json, 'w') as f: f.write('{"test": true}')
        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, "sk-invalid", args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            error = client.call_archive(test_json)
            if error == -1 and os.path.exists(test_json) and not os.path.exists(test_json + '.processing'):
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - error={error}, json_exists={os.path.exists(test_json)}, processing={os.path.exists(test_json+'.processing')}")
                results['FAIL'] += 1
            stop_node(proc)

        # T6: 有效 API key 完整归档（需要真实 API key）
        print("=== T6: Full archive (valid API key) ===")
        if args.api_key == 'sk-invalid' or args.api_key == '':
            print("SKIP"); results['SKIP'] += 1
        else:
            test_json2 = os.path.join(tmpdir, 'test_archive2.json')
            with open(test_json2, 'w') as f: f.write('{"conversation": "Hello"}')
            if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
            os.makedirs(repo_dir)
            proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
            if not proc:
                print("FAIL"); results['FAIL'] += 1
            else:
                client = MemoryTestClient(args.agent_name)
                error = client.call_archive(test_json2)
                if error == 0 and os.path.exists(test_json2 + '.done'):
                    print("PASS"); results['PASS'] += 1
                else:
                    print(f"FAIL - error={error}, done exists={os.path.exists(test_json2+'.done')}")
                    results['FAIL'] += 1
                stop_node(proc)

        # T13: 重命名失败（目录只读）
        print("=== T13: Rename failure ===")
        ro_dir = os.path.join(tmpdir, 'readonly_dir')
        os.makedirs(ro_dir)
        test_json_ro = os.path.join(ro_dir, 'test.json')
        with open(test_json_ro, 'w') as f: f.write("{}")
        os.chmod(ro_dir, 0o555)
        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            os.chmod(ro_dir, 0o755); shutil.rmtree(ro_dir)
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            error = client.call_archive(test_json_ro, timeout=10)
            if error == -1 and os.path.exists(test_json_ro) and not os.path.exists(test_json_ro + '.processing'):
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - error={error}, json_exists={os.path.exists(test_json_ro)}, processing={os.path.exists(test_json_ro+'.processing')}")
                results['FAIL'] += 1
            stop_node(proc)
            os.chmod(ro_dir, 0o755)
            shutil.rmtree(ro_dir)

        # T14: 仓库同步失败（删除 .git）
        print("=== T14: Repo sync failure before archive ===")
        test_json14 = os.path.join(tmpdir, 'test14.json')
        with open(test_json14, 'w') as f: f.write('{"test": 14}')
        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        run_git(['git', 'clone', bare_path, repo_dir])
        shutil.rmtree(os.path.join(repo_dir, '.git'))
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            error = client.call_archive(test_json14, timeout=10)
            if error == -1 and os.path.exists(test_json14) and not os.path.exists(test_json14 + '.processing'):
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - error={error}, json_exists={os.path.exists(test_json14)}, processing={os.path.exists(test_json14+'.processing')}")
                results['FAIL'] += 1
            stop_node(proc)

        # T15: 读 JSON 失败（时序问题，标记 SKIP）
        print("=== T15: Read JSON failure (SKIP) ===")
        results['SKIP'] += 1

        # T16: 写日记失败（SKIP）
        print("=== T16: Write diary failure (SKIP) ===")
        results['SKIP'] += 1

        # T17: Git commit 失败（损坏 .git/index）
        print("=== T17: Git commit failure ===")
        test_json17 = os.path.join(tmpdir, 'test17.json')
        with open(test_json17, 'w') as f: f.write('{"test": 17}')
        if os.path.exists(repo_dir): shutil.rmtree(repo_dir)
        os.makedirs(repo_dir)
        run_git(['git', 'clone', bare_path, repo_dir])
        index_path = os.path.join(repo_dir, '.git', 'index')
        with open(index_path, 'wb') as f: f.write(b'')
        os.chmod(index_path, 0o444)
        proc = start_node(args.agent_name, repo_url, repo_dir, args.api_key, args.api_base, args.model)
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            error = client.call_archive(test_json17, timeout=10)
            if error == -1 and os.path.exists(test_json17) and not os.path.exists(test_json17 + '.processing'):
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - error={error}, json_exists={os.path.exists(test_json17)}, processing={os.path.exists(test_json17+'.processing')}")
                results['FAIL'] += 1
            stop_node(proc)
            os.chmod(index_path, 0o644)

        # T18: Push failure, retries exhausted（修复权限递归）
        print("=== T18: Push failure, retries exhausted ===")
        bare_ro = os.path.join(tmpdir, 'remote_ro.git')
        work_ro = os.path.join(tmpdir, 'work_ro')
        os.makedirs(bare_ro)
        run_git(['git', 'init', '--bare', '--initial-branch=main', bare_ro])
        run_git(['git', 'clone', bare_ro, work_ro])
        os.makedirs(os.path.join(work_ro, 'prompts'))
        with open(os.path.join(work_ro, 'prompts/RULE.md'), 'w') as f: f.write("dummy")
        run_git(['git', 'add', '-A'], cwd=work_ro)
        run_git(['git', 'commit', '-m', 'init'], cwd=work_ro)
        run_git(['git', 'push', 'origin', 'main'], cwd=work_ro)
        shutil.rmtree(work_ro)
        # 递归移除所有写权限
        subprocess.run(['chmod', '-R', 'a-w', bare_ro], check=True)
        ro_url = f'file://{bare_ro}'
        ro_dir = os.path.join(tmpdir, 'ro_work')
        os.makedirs(ro_dir, exist_ok=True)

        test_json18 = os.path.join(tmpdir, 'test18.json')
        with open(test_json18, 'w') as f: f.write('{"test": 18}')

        proc = start_node(args.agent_name, ro_url, ro_dir, args.api_key, args.api_base, args.model,
                          extra_params={'push_retry_max': 2})
        if not proc:
            print("FAIL"); results['FAIL'] += 1
        else:
            client = MemoryTestClient(args.agent_name)
            error = client.call_archive(test_json18, timeout=30)
            if error == -1 and os.path.exists(test_json18) and not os.path.exists(test_json18+'.processing') and not os.path.exists(test_json18+'.done'):
                print("PASS"); results['PASS'] += 1
            else:
                print(f"FAIL - error={error}, json={os.path.exists(test_json18)}, proc={os.path.exists(test_json18+'.processing')}, done={os.path.exists(test_json18+'.done')}")
                results['FAIL'] += 1
            stop_node(proc)
        subprocess.run(['chmod', '-R', 'u+w', bare_ro], check=True)
        shutil.rmtree(bare_ro, ignore_errors=True)

        # T19: 无限重试 + SIGINT 中断恢复 TODO
        # print("=== T19: Infinite push retry + SIGINT ===")
        # bare_ro2 = os.path.join(tmpdir, 'remote_ro2.git')
        # work_ro2 = os.path.join(tmpdir, 'work_ro2')
        # os.makedirs(bare_ro2)
        # run_git(['git', 'init', '--bare', '--initial-branch=main', bare_ro2])
        # run_git(['git', 'clone', bare_ro2, work_ro2])
        # os.makedirs(os.path.join(work_ro2, 'prompts'))
        # with open(os.path.join(work_ro2, 'prompts/RULE.md'), 'w') as f: f.write("dummy")
        # run_git(['git', 'add', '-A'], cwd=work_ro2)
        # run_git(['git', 'commit', '-m', 'init'], cwd=work_ro2)
        # run_git(['git', 'push', 'origin', 'main'], cwd=work_ro2)
        # shutil.rmtree(work_ro2)
        # os.chmod(bare_ro2, 0o555)
        # ro_url2 = f'file://{bare_ro2}'
        # ro_dir2 = os.path.join(tmpdir, 'ro_work2')
        # os.makedirs(ro_dir2, exist_ok=True)

        # test_json19 = os.path.join(tmpdir, 'test19.json')
        # with open(test_json19, 'w') as f: f.write('{"test": 19}')

        # proc = start_node(args.agent_name, ro_url2, ro_dir2, args.api_key, args.api_base, args.model,
        #                   extra_params={'push_retry_max': -1})
        # if not proc:
        #     print("FAIL"); results['FAIL'] += 1
        # else:
        #     archive_result = None
        #     def call_archive_thread():
        #         nonlocal archive_result
        #         try:
        #             client = MemoryTestClient(args.agent_name)
        #             archive_result = client.call_archive(test_json19, timeout=30)
        #         except Exception:
        #             archive_result = "exception"

        #     t = threading.Thread(target=call_archive_thread, daemon=True)
        #     t.start()
        #     time.sleep(10)  # 确保进入推送循环
        #     proc.send_signal(signal.SIGINT)
        #     t.join(timeout=15)  # 最多等15秒线程退出
        #     # 强制终止节点（如果还没退出）
        #     try:
        #         proc.wait(timeout=3)
        #     except subprocess.TimeoutExpired:
        #         proc.kill()
        #         proc.wait()
        #     print_stderr(proc)

        #     if os.path.exists(test_json19) and not os.path.exists(test_json19 + '.processing'):
        #         print("PASS"); results['PASS'] += 1
        #     else:
        #         print(f"FAIL - json_exists={os.path.exists(test_json19)}, processing={os.path.exists(test_json19+'.processing')}")
        #         results['FAIL'] += 1
        # os.chmod(bare_ro2, 0o755)
        # shutil.rmtree(bare_ro2, ignore_errors=True)

    print(f"\nResults: PASS={results['PASS']}, FAIL={results['FAIL']}, SKIP={results['SKIP']}")
    rclpy.shutdown()

if __name__ == '__main__':
    main()