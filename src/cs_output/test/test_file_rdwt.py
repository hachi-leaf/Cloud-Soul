#!/usr/bin/env python3
import subprocess, json, sys, re
if len(sys.argv) < 2:
    print('Usage: python3 test_file_rdwt.py <agent_name> [--verbose]')
    sys.exit(1)
agent = sys.argv[1]
VERBOSE = '--verbose' in sys.argv
PASS, FAIL = 0, 0
AS = '/' + agent + '/output/file_rdwt'

def ra(gj):
    g = json.dumps({'input_json': json.dumps(gj), 'timeout_sec': 10.0})
    r = subprocess.run(['ros2','action','send_goal',AS,'cs_interfaces/action/ExecuteTool',g],
        capture_output=True, text=True, timeout=15)
    m = re.search(r"output_json:\s*'(.+?)'", r.stdout)
    if not m: return None, None
    try: out = json.loads(m.group(1))
    except: return m.group(1), None
    m2 = re.search(r'exit_code:\s*(-?\d+)', r.stdout)
    return out, int(m2.group(1)) if m2 else None

def sr(p):
    o, e = ra({'action':'read','path':p})
    return o.get('content','') if o else '', e

def sw(p, c, m='overwrite'):
    return ra({'action':'write','path':p,'content':c,'mode':m})

def t(n, c):
    global PASS, FAIL
    if c: PASS += 1
    else: FAIL += 1; print(f'  FAIL: {n}')

def s(title):
    print(f'\n{"="*60}\n  {title}\n{"="*60}')
