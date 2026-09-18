# M4 端到端：权限闸门走真实 HTTP，完全按界面的方式来。
#
# 单元测试证明的是"闸门在库里能拦住"。这一份证明的是另一件事：
# **那道闸门经过 REST + SSE 之后仍然拦得住**——事件带得出 permissionID、
# 裁决回得去、拒绝之后文件确实没被写出来。
#
# 一切收发都在 Python 里显式 UTF-8，不经过 shell 传参——
# 这台机器的 bash/PowerShell 会把中文按 GBK 毁掉，之前两次误判服务端有 bug。
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

out = sys.stdout.buffer
fails = []


def say(m):
    out.write((m + "\n").encode("utf-8"))
    out.flush()


def check(name, cond, extra=""):
    say("  [%s] %s%s" % ("OK" if cond else "FAIL", name, ("  " + extra) if extra else ""))
    if not cond:
        fails.append(name)


# ── 假模型：按剧本依次返回"要调 write"和"最终回答" ────────────
SCRIPTS = []          # 每个元素: (text, tool_name, tool_args)
served = {"n": 0}
served_lock = threading.Lock()


class ModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        with served_lock:
            idx = served["n"]
            served["n"] += 1
            ModelHandler.bodies.append(body)
        script = SCRIPTS[idx] if idx < len(SCRIPTS) else ("", "", "")
        text, tool, args = script

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        if tool:
            frame = {"choices": [{"delta": {"tool_calls": [{
                "index": 0,
                "id": "call_%d" % idx,
                "function": {"name": tool, "arguments": args},
            }]}}]}
            self.wfile.write(("data: " + json.dumps(frame, ensure_ascii=False) + "\n\n")
                             .encode("utf-8"))
        if text:
            frame = {"choices": [{"delta": {"content": text}}]}
            self.wfile.write(("data: " + json.dumps(frame, ensure_ascii=False) + "\n\n")
                             .encode("utf-8"))
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


ModelHandler.bodies = []
model = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
model_port = model.server_address[1]
threading.Thread(target=model.serve_forever, daemon=True).start()
say("假模型监听在 127.0.0.1:%d" % model_port)

workspace = tempfile.mkdtemp(prefix="maiagent-m4-")
say("工作目录 " + workspace)

proc = subprocess.Popen(
    [r"E:\OpenSource\multi-ai-code\MaiAgent\build\bin\maiagent-bridge.exe",
     "--model-url", "http://127.0.0.1:%d" % model_port,
     "--model-key", "fake", "--model", "glm-5.3"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

base = None
deadline = time.time() + 15
while time.time() < deadline:
    line = proc.stdout.readline().decode("utf-8", "replace")
    if not line:
        break
    m = re.search(r"(http://127\.0\.0\.1:\d+)", line)
    if m:
        base = m.group(1)
        break
if not base:
    say("服务端没起来")
    sys.exit(1)
say("maiagent-bridge 监听在 " + base)
say("")


def get(path):
    with urllib.request.urlopen(base + path, timeout=10) as r:
        return r.status, r.read()


def post(path, obj):
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8") if obj is not None else b""
    req = urllib.request.Request(
        base + path, data=data,
        headers={"Content-Type": "application/json; charset=utf-8"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


# ── 像界面那样挂一条 SSE，全程不断开 ─────────────────────────
frames = []
lock = threading.Lock()
stop = threading.Event()


def sse():
    try:
        with urllib.request.urlopen(base + "/api/event", timeout=120) as r:
            buf = b""
            while not stop.is_set():
                ch = r.read(1)
                if not ch:
                    break
                buf += ch
                if buf.endswith(b"\n\n"):
                    for ln in buf.decode("utf-8", "replace").split("\n"):
                        if ln.startswith("data: ") and not ln.startswith("data: :"):
                            try:
                                with lock:
                                    frames.append(json.loads(ln[6:]))
                            except ValueError:
                                pass
                    buf = b""
    except Exception:
        pass


threading.Thread(target=sse, daemon=True).start()
time.sleep(0.4)


def events(kind):
    with lock:
        return [f for f in frames if f.get("type") == kind]


def wait_for(fn, limit=10.0):
    end = time.time() + limit
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.05)
    return fn()


def new_session():
    st, body = post("/api/session", {"directory": workspace})
    return json.loads(body.decode("utf-8"))["id"]


# ══ 1. 拒绝：文件不该出现 ═════════════════════════════════════
say("== 1. 拒绝一次 write ==")
SCRIPTS[:] = [("", "write", json.dumps({"path": "rejected.txt", "content": "不该出现"})),
              ("好的，我不写了。", "", "")]
with served_lock:
    served["n"] = 0

sid = new_session()
post("/api/session/%s/prompt" % sid, {"text": "写 rejected.txt"})

pending = wait_for(lambda: json.loads(get("/api/permission")[1].decode("utf-8")))
check("GET /api/permission 列出了待裁决", len(pending) == 1,
      "%d 条" % len(pending))
if pending:
    p = pending[0]
    check("带上了工具名", p.get("tool") == "write", repr(p.get("tool")))
    check("带上了参数原文", "rejected.txt" in p.get("input", ""))
    check("id 以 per_ 开头", str(p.get("id", "")).startswith("per_"), p.get("id"))
    check("partID 指向工具卡", bool(p.get("partID")))

asked = wait_for(lambda: events("permission.asked"))
check("SSE 收到 permission.asked", len(asked) == 1)
if asked and pending:
    check("事件里带了 permissionID",
          asked[0]["data"].get("permissionID") == pending[0]["id"],
          str(asked[0]["data"].get("permissionID")))
    check("事件里带了 partID", asked[0]["data"].get("partID") == pending[0]["partID"])

# 等授权期间刷新界面也要看得见那张卡（SSE 断线重连就是这个场景）。
msgs = json.loads(get("/api/session/%s/message" % sid)[1].decode("utf-8"))
tool_parts = [pp for m in msgs for pp in m["parts"] if pp.get("type") == "tool"]
check("刷新也能拉到工具卡", len(tool_parts) == 1, "%d 个" % len(tool_parts))
if tool_parts:
    check("状态是 pending", tool_parts[0]["state"]["status"] == "pending",
          tool_parts[0]["state"]["status"])

check("这一刻文件还不存在", not os.path.exists(os.path.join(workspace, "rejected.txt")))

st, body = post("/api/permission/%s" % pending[0]["id"], {"decision": "reject"})
check("裁决返回 200", st == 200, str(st))

wait_for(lambda: events("session.idle"))
time.sleep(0.3)
check("文件确实没被写出来", not os.path.exists(os.path.join(workspace, "rejected.txt")))

replied = events("permission.replied")
check("SSE 收到 permission.replied", len(replied) == 1)
if replied:
    check("裁决内容是 reject", replied[0]["data"].get("detail") == "reject",
          str(replied[0]["data"].get("detail")))

fed = ""
for b in ModelHandler.bodies:
    j = json.loads(b.decode("utf-8"))
    for m in j.get("messages", []):
        if m.get("role") == "tool":
            fed = m.get("content", "")
check("拒绝的理由回灌给了模型", "拒绝" in fed, repr(fed[:40]))
check("并且明确告诉它别重试", "不要重试" in fed)
say("")

# ══ 2. 允许一次：文件该出现 ═══════════════════════════════════
say("== 2. 允许一次 write ==")
with lock:
    frames.clear()
SCRIPTS[:] = [("", "write", json.dumps({"path": "allowed.txt", "content": "批准之后才出现"})),
              ("写好了。", "", "")]
with served_lock:
    served["n"] = 0
    ModelHandler.bodies[:] = []

sid2 = new_session()
post("/api/session/%s/prompt" % sid2, {"text": "写 allowed.txt"})

pending = wait_for(lambda: json.loads(get("/api/permission")[1].decode("utf-8")))
check("又有一条在等", len(pending) == 1)
st, body = post("/api/permission/%s" % pending[0]["id"], {"decision": "once"})
check("裁决返回 200", st == 200, str(st))

wait_for(lambda: events("session.idle"))
time.sleep(0.3)
target = os.path.join(workspace, "allowed.txt")
check("文件写出来了", os.path.exists(target))
if os.path.exists(target):
    with open(target, "rb") as f:
        got = f.read().decode("utf-8")
    check("内容正确且中文没坏", got == "批准之后才出现", repr(got))

msgs = json.loads(get("/api/session/%s/message" % sid2)[1].decode("utf-8"))
tool_parts = [pp for m in msgs for pp in m["parts"] if pp.get("type") == "tool"]
check("工具卡变成 completed",
      bool(tool_parts) and tool_parts[0]["state"]["status"] == "completed",
      tool_parts[0]["state"]["status"] if tool_parts else "没有")
check("待裁决清空了", json.loads(get("/api/permission")[1].decode("utf-8")) == [])
say("")

# ══ 3. 认不出的 decision 必须 400，不能兜底成放行 ═════════════
say("== 3. 坏的 decision ==")
with lock:
    frames.clear()
SCRIPTS[:] = [("", "write", json.dumps({"path": "typo.txt", "content": "x"})),
              ("结束。", "", "")]
with served_lock:
    served["n"] = 0

sid3 = new_session()
post("/api/session/%s/prompt" % sid3, {"text": "写 typo.txt"})
pending = wait_for(lambda: json.loads(get("/api/permission")[1].decode("utf-8")))
check("有一条在等", len(pending) == 1)

pid = pending[0]["id"]
for bad in ["Once", "yes", "", "ALLOW"]:
    st, body = post("/api/permission/%s" % pid, {"decision": bad})
    check("decision=%r 被拒成 400" % bad, st == 400, str(st))

check("那一条还在等，没被误放行",
      len(json.loads(get("/api/permission")[1].decode("utf-8"))) == 1)
check("文件仍然不存在", not os.path.exists(os.path.join(workspace, "typo.txt")))

# 不存在的 id 要 404，而不是假装成功
st, _ = post("/api/permission/per_nope", {"decision": "once"})
check("不存在的 permissionID 返回 404", st == 404, str(st))

# 用中断收尾，顺带验证卡在等授权的那一轮能被叫醒
st, _ = post("/api/session/%s/interrupt" % sid3, {})
check("interrupt 返回 200", st == 200, str(st))
idle = wait_for(lambda: events("session.idle"), limit=10)
check("等授权的那一轮被中断唤醒了", bool(idle))
check("待裁决被清掉", json.loads(get("/api/permission")[1].decode("utf-8")) == [])
say("")

# ══ 4. always：同一会话第二次不再问 ═══════════════════════════
say("== 4. always 之后不再问 ==")
with lock:
    frames.clear()
SCRIPTS[:] = [("", "write", json.dumps({"path": "one.txt", "content": "1"})),
              ("", "write", json.dumps({"path": "two.txt", "content": "2"})),
              ("两个都写好了。", "", "")]
with served_lock:
    served["n"] = 0

sid4 = new_session()
post("/api/session/%s/prompt" % sid4, {"text": "写两个文件"})
pending = wait_for(lambda: json.loads(get("/api/permission")[1].decode("utf-8")))
check("第一次问了", len(pending) == 1)
post("/api/permission/%s" % pending[0]["id"], {"decision": "always"})

wait_for(lambda: events("session.idle"))
time.sleep(0.3)
check("第一个文件写了", os.path.exists(os.path.join(workspace, "one.txt")))
check("第二个文件也写了", os.path.exists(os.path.join(workspace, "two.txt")))
check("全程只问了一次", len(events("permission.asked")) == 1,
      "%d 次" % len(events("permission.asked")))
say("")

# ── 收尾 ──────────────────────────────────────────────────────
stop.set()
proc.terminate()
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()
model.shutdown()
shutil.rmtree(workspace, ignore_errors=True)

say("-" * 40)
if fails:
    say("M4 端到端有 %d 项失败：" % len(fails))
    for f in fails:
        say("  - " + f)
    sys.exit(1)
say("M4 端到端全部通过")
