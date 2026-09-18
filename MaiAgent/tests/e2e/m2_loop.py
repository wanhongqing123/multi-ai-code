# M2 端到端：起一个假模型 + 真的 maiagent-bridge，完全按界面的方式走一遍。
#
# 一切收发都在 Python 里显式 UTF-8，不经过 shell 传参——
# 这台机器的 bash/PowerShell 会把中文按 GBK 毁掉，之前两次误判服务端有 bug。
import json
import re
import subprocess
import sys
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


# ── 假模型：一个真的 Chat Completions 端点，慢慢吐字 ──────────
# 载荷，不是文案：验证多字节字符被切在 SSE chunk 边界上还能拼回来。
# 合起来是 "你好！我是跑在 C++ 核心里的测试模型 🙂"
CHUNKS = ["\u4f60\u597d", "\uff01\u6211\u662f", "\u8dd1\u5728 C++ ",
          "\u6838\u5fc3\u91cc\u7684", "\u6d4b\u8bd5\u6a21\u578b \U0001F642"]
FULL = "".join(CHUNKS)


class ModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        ModelHandler.last_body = self.rfile.read(n)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for c in CHUNKS:
            frame = "data: " + json.dumps(
                {"choices": [{"delta": {"content": c}}]}, ensure_ascii=False) + "\n\n"
            self.wfile.write(frame.encode("utf-8"))
            self.wfile.flush()
            time.sleep(0.06)
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


ModelHandler.last_body = b""
model = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
model_port = model.server_address[1]
threading.Thread(target=model.serve_forever, daemon=True).start()
say("fake model listening on 127.0.0.1:%d" % model_port)

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
    say("bridge did not start")
    sys.exit(1)
say("maiagent-bridge listening on " + base)
say("")


def get(path):
    with urllib.request.urlopen(base + path, timeout=10) as r:
        return r.status, r.read()


def post(path, obj):
    req = urllib.request.Request(
        base + path, data=json.dumps(obj, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json; charset=utf-8"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return r.status, r.read()


# ── 像界面那样挂一条 SSE，全程不断开 ─────────────────────────
frames = []
lock = threading.Lock()
stop = threading.Event()


def sse():
    try:
        with urllib.request.urlopen(base + "/api/event", timeout=60) as r:
            buf = b""
            while not stop.is_set():
                ch = r.read(1)
                if not ch:
                    break
                buf += ch
                if buf.endswith(b"\n\n"):
                    ln = buf.strip()
                    if ln.startswith(b"data: "):
                        with lock:
                            frames.append(json.loads(ln[6:].decode("utf-8")))
                    buf = b""
    except Exception:
        pass


threading.Thread(target=sse, daemon=True).start()
time.sleep(0.5)


def idle_count():
    with lock:
        return sum(1 for f in frames if f["type"] == "session.idle")


def wait_turns(n, timeout=20):
    """等到第 n 次 session.idle。轮次结束的唯一可靠信号就是它——
    只等消息落库是不够的，那时候 turn 还在做收尾（改标题、销毁线程）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if idle_count() >= n:
            return True
        time.sleep(0.03)
    return False


def snap():
    with lock:
        return list(frames)


say("== 1. create session ==")
st, raw = post("/api/session", {"title": "", "directory": "E:/tmp"})
sid = json.loads(raw.decode("utf-8"))["id"]
check("session created", sid.startswith("ses_"), sid)

say("== 2. send a prompt (must return at once, not wait for the model) ==")
t0 = time.time()
st, raw = post("/api/session/%s/prompt" % sid, {"text": "who are you?"})
elapsed = time.time() - t0
msg_id = json.loads(raw.decode("utf-8"))["messageID"]
check("returned a messageID", msg_id.startswith("msg_"), msg_id)
check("returned immediately (<0.3s)", elapsed < 0.3, "%.3fs" % elapsed)

say("== 3. wait for the turn to finish ==")
check("got session.idle", wait_turns(1))
fs = snap()
types = [f["type"] for f in fs]
deltas = [f for f in fs if f["type"] == "message.part.delta"]
check("got %d deltas" % len(deltas), len(deltas) == len(CHUNKS), "expected %d" % len(CHUNKS))
check("no session.error", "session.error" not in types)

say("== 4. reassemble the deltas (exactly what the UI does) ==")
part_ids = {d["data"]["partID"] for d in deltas}
check("part id stayed stable", len(part_ids) == 1, str(part_ids))
assembled = "".join(d["data"]["delta"] for d in deltas)
check("reassembled == full text", assembled == FULL, repr(assembled))

say("== 5. stored content matches (a refresh shows the same thing) ==")
msgs = json.loads(get("/api/session/%s/message" % sid)[1].decode("utf-8"))
check("two messages", len(msgs) == 2, str(len(msgs)))
if len(msgs) == 2:
    check("first is user", msgs[0]["role"] == "user")
    check("second is assistant", msgs[1]["role"] == "assistant")
    txt = "".join(p.get("text", "") for p in msgs[1]["parts"])
    check("stored text == streamed text", txt == FULL, repr(txt))
    check("stored part id == the one in the events", msgs[1]["parts"][0]["id"] in part_ids)

say("== 6. title is taken from the first prompt ==")
title = json.loads(get("/api/session/" + sid)[1].decode("utf-8"))["title"]
check("title == the first prompt", title == "who are you?", repr(title))

say("== 7. the request body sent to the model ==")
b = json.loads(ModelHandler.last_body.decode("utf-8"))
check("model is correct", b.get("model") == "glm-5.3", repr(b.get("model")))
check("stream=true", b.get("stream") is True)
check("carries the user prompt", b["messages"][-1]["content"] == "who are you?")

say("== 8. second turn carries the full history ==")
post("/api/session/%s/prompt" % sid, {"text": "say that again"})
check("second turn finished", wait_turns(2))
b = json.loads(ModelHandler.last_body.decode("utf-8"))
check("three messages in history", len(b["messages"]) == 3, str(len(b["messages"])))
if len(b["messages"]) == 3:
    check("includes the previous answer", b["messages"][1]["content"] == FULL)

say("== 9. the parts-shaped body is accepted too ==")
st, raw = post("/api/session/%s/prompt" % sid,
               {"parts": [{"type": "text", "text": "sent via parts"}]})
check("parts shape accepted", json.loads(raw.decode("utf-8"))["messageID"].startswith("msg_"))
check("third turn finished", wait_turns(3))
b = json.loads(ModelHandler.last_body.decode("utf-8"))
check("text from parts reached the model", b["messages"][-1]["content"] == "sent via parts",
      repr(b["messages"][-1]["content"]))

say("== 10. a busy session rejects a second prompt "
    "(no queueing: queued messages look lost to the user) ==")
post("/api/session/%s/prompt" % sid, {"text": "answer slowly"})
busy_rejected = False
try:
    post("/api/session/%s/prompt" % sid, {"text": "cut in line"})
except urllib.error.HTTPError as e:
    busy_rejected = (e.code == 409)
check("rejected with 409", busy_rejected)
check("fourth turn finished", wait_turns(4))

say("== 11. an empty prompt gives 400 ==")
try:
    post("/api/session/%s/prompt" % sid, {"text": ""})
    check("400", False, "it succeeded instead")
except urllib.error.HTTPError as e:
    check("400", e.code == 400, str(e.code))

say("== 12. interrupt ==")
sid2 = json.loads(post("/api/session", {"title": ""})[1].decode("utf-8"))["id"]
before = idle_count()
post("/api/session/%s/prompt" % sid2, {"text": "count for me"})
time.sleep(0.1)  # 让它吐出一两个 chunk
st, raw = post("/api/session/%s/interrupt" % sid2, {})
check("interrupt returned true", json.loads(raw.decode("utf-8"))["interrupted"] is True)
check("went idle after the interrupt", wait_turns(before + 1))
m2 = json.loads(get("/api/session/%s/message" % sid2)[1].decode("utf-8"))
check("the partial content was stored", len(m2) == 2 and len(m2[1]["parts"]) >= 1)

stop.set()
proc.terminate()
model.shutdown()

say("")
if fails:
    say("%d checks failed: %s" % (len(fails), ", ".join(fails)))
    sys.exit(1)
say("M2 end-to-end: all checks passed")
