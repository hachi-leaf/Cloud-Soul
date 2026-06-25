#!/usr/bin/env python3
"""file_rdwt_node exhaustive test. Usage: python3 test_file_rdwt.py <agent_name> [--verbose]"""
import subprocess, json, sys, re, os
if len(sys.argv) < 2:
    print("Usage: python3 test_file_rdwt.py <agent_name> [--verbose]")
    sys.exit(1)
agent = sys.argv[1]
VERBOSE = "--verbose" in sys.argv
PASS, FAIL = 0, 0
AS = "/" + agent + "/output/file_rdwt"

def get_output_json(stdout):
    prefix = "output_json: '"
    i = stdout.find(prefix)
    if i < 0: return None, None
    i += len(prefix)
    j = stdout.find("'\nexit_code:", i)
    if j < 0: j = stdout.find("'exit_code:", i)
    if j < 0: return None, None
    raw = stdout[i:j].replace("\\'","'")
    try: out = json.loads(raw)
    except: out = raw
    m = re.search(r"exit_code:\s*(-?\d+)", stdout)
    ec = int(m.group(1)) if m else None
    return out, ec

def ra(gj):
    g = json.dumps({"input_json": json.dumps(gj), "timeout_sec": 10.0})
    r = subprocess.run(["ros2","action","send_goal",AS,"cs_interfaces/action/ExecuteTool",g], capture_output=True, text=True, timeout=15)
    return get_output_json(r.stdout)

def sr(p):
    o, e = ra({"action":"read","path":p})
    return (o.get("content","") if isinstance(o,dict) else ""), e

def sw(p, c, m="overwrite"):
    return ra({"action":"write","path":p,"content":c,"mode":m})

def t(n, c):
    global PASS, FAIL
    if c: PASS += 1
    else: FAIL += 1; print("  FAIL: " + n)

def sect(title):
    print()
    print("="*60)
    print("  " + title)
    print("="*60)

# ================================================================
sect("1. Basic Write + Read")

f1 = "/tmp/rt1.txt"
o, e = sw(f1, "hello world")
t("small write", e == 0 and o.get("written") == 11)
o, e = ra({"action":"read","path":f1})
t("small read", e == 0 and o.get("content") == "hello world")
t("size", o.get("size") == 11)
o, e = sw("/tmp/rte.txt", "")
t("empty write rejected", e == -1)

# ================================================================
sect("2. Write Modes")
f2 = "/tmp/rt2.txt"
sw(f2, "L1\nL2\nL3\n")
o, e = sw(f2, "new", "overwrite")
d, _ = sr(f2); t("overwrite", d == "new")
sw(f2, "a\nb\n")
o, e = sw(f2, "c\n", "append")
d, _ = sr(f2); t("append", d == "a\nb\nc\n")
sw(f2, "x\ny\nz\n")
o, e = ra({"action":"write","path":f2,"content":"I\n","mode":"insert","range":{"start_line":2}})
d, _ = sr(f2); t("insert mid", d == "x\nI\ny\nz\n")
o, e = ra({"action":"write","path":f2,"content":"E\n","mode":"insert","range":{"start_line":100}})
d, _ = sr(f2); t("insert beyond", d == "x\nI\ny\nz\nE\n")
o, e = ra({"action":"write","path":f2,"content":"T\n","mode":"insert","range":{"start_line":0}})
t("line0 rejected", e == -1)

# ================================================================
sect("3. Line Ranges")
f3 = "/tmp/rt3.txt"
sw(f3, "L1\nL2\nL3\nL4\nL5\n")
o, e = ra({"action":"read","path":f3,"range":{"start_line":2,"end_line":2}})
t("single line", o.get("content") == "L2\n")
o, e = ra({"action":"read","path":f3,"range":{"start_line":2,"end_line":4}})
t("range 2-4", o.get("content") == "L2\nL3\nL4\n")
o, e = ra({"action":"read","path":f3,"range":{"start_line":3,"end_line":-1}})
t("to end", o.get("content") == "L3\nL4\nL5\n")
o, e = ra({"action":"read","path":f3,"range":{"start_line":4,"end_line":10}})
t("clamped", o.get("content") == "L4\nL5\n")
o, e = ra({"action":"read","path":f3,"range":{"start_line":5,"end_line":2}})
t("start>end rejected", e == -1)
o, e = ra({"action":"read","path":f3,"range":{"start_line":10,"end_line":20}})
t("beyond empty", o.get("content") == "")

# ================================================================
sect("4. Large HTML with Special Characters")
SQ = chr(39)
html = "<!DOCTYPE html>\n<html><body><p>"
html += SQ + chr(44) + SQ
html += SQ + chr(125) + SQ
html += SQ + chr(93) + SQ
html += SQ + chr(58) + SQ
html += "</p></body></html>"

f4 = "/tmp/rt4.html"
o, e = sw(f4, html)
t("HTML write", e == 0)
t("HTML bytes", o.get("written") == len(html))
with open(f4, "rb") as f: disk = f.read()
t("HTML roundtrip", disk == html.encode())

# ================================================================
sect("5. read_write Action")
f5 = "/tmp/rt5.txt"
sw(f5, "original")
o, e = ra({"action":"read_write","path":f5,"content":"new","mode":"overwrite"})
t("RW succeeded", e == 0)
t("RW returns written", o.get("content","") == "new" if isinstance(o,dict) else False)
t("RW bytes", o.get("written") == 3 if isinstance(o,dict) else False)
d, _ = sr(f5); t("RW on disk", d == "new")

# ================================================================
sect("6. Error Handling")
o, e = ra({"action":"read","path":"/tmp/noexist_xyz"}); t("not found", e == -1)
o, e = ra({"action":"read","path":"/tmp"}); t("directory", e == -1)
o, e = ra({"action":"read","path":"relative/path.txt"}); t("relative path", e == -1)
o, e = ra({"action":"read","path":"/tmp/../etc/passwd"}); t("dotdot path", e == -1)
o, e = ra({"action":"bad","path":"/tmp/x"}); t("bad action", e == -1)
o, e = ra({"action":"write","path":"/tmp/x","content":"x","mode":"xyzzy"}); t("bad mode", e == -1)
o, e = ra({"action":"write","path":"/tmp/x","mode":"overwrite"}); t("no content", e == -1)

# ================================================================
sect("7. 100KB Stress")
big = "x" * 100000
f7 = "/tmp/rt7.txt"
o, e = sw(f7, big)
t("100KB write", e == 0)
t("100KB bytes", o.get("written") == 100000)
with open(f7, "rb") as f: disk = f.read()
t("100KB roundtrip", disk == big.encode())

# ================================================================
sect("8. Unicode + Binary")
tc = "Unicode: \u4e2d\u6587\u65e5\u672c\u8a9e\ud55c\uad6d\uc5b4 \U0001f30d\U0001f525\n"
tc += "Binary-ish: \x00\x01\x02\x7f\x80\xff\n"
tc += 'JSON: {"key":"value","arr":[1,2,3]}\n'
tc += "Shell: $HOME `pwd` > /dev/null &\n"
tc += "SQL: SELECT * FROM users WHERE name=''admin'' OR 1=1 --\n"
f8 = "/tmp/rt8.txt"
o, e = sw(f8, tc)
t("unicode write", e == 0)
with open(f8, "rb") as f: disk = f.read()
t("unicode roundtrip", disk == tc.encode())

# ================================================================
print()
print("="*60)
total = PASS + FAIL
# ==============================================================
sect('9. JSON Repair (repair_json)')
# repair_json auto-fixes malformed action JSON (LLM output errors)
# Tests: trailing commas, missing braces, extra text in input_json

f9 = '/tmp/rt_r1.txt'
o, e = ra({action:"write",path:f9,content:'ok',mode:"overwrite"})
t('baseline write', e == 0)
d, _ = sr(f9); t('baseline readback', d == 'ok')

print("  Results: %d/%d passed, %d failed" % (PASS, total, FAIL))
if FAIL == 0: print("  ALL TESTS PASSED")
else: print("  %d FAILURES DETECTED" % FAIL)
print("="*60)
sys.exit(0 if FAIL == 0 else 1)
# ==============================================================
