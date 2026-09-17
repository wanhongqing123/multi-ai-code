# M2 端到端：起一个假模型 + 真的 maiagent-server，完全按界面的方式走一遍。
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
CHUNKS = ["你好", "！我是", "跑在 C++ ", "核心里的", "测试模型 🙂"]
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
say("假模型监听在 127.0.0.1:%d" % model_port)

proc = subprocess.Popen(
    [r"E:\OpenSource\multi-ai-code\MaiAgent\build\bin\maiagent-server.exe",
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
say("maiagent-server 监听在 " + base)
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


say("== 1. 建会话 ==")
st, raw = post("/api/session", {"title": "", "directory": "E:/tmp"})
sid = json.loads(raw.decode("utf-8"))["id"]
check("会话建好", sid.startswith("ses_"), sid)

say("== 2. 发消息（应立刻返回，不等模型）==")
t0 = time.time()
st, raw = post("/api/session/%s/prompt" % sid, {"text": "你是谁？"})
elapsed = time.time() - t0
msg_id = json.loads(raw.decode("utf-8"))["messageID"]
check("返回 messageID", msg_id.startswith("msg_"), msg_id)
check("立刻返回（<0.3s）", elapsed < 0.3, "%.3fs" % elapsed)

say("== 3. 等这一轮吐完 ==")
check("收到 session.idle", wait_turns(1))
fs = snap()
types = [f["type"] for f in fs]
deltas = [f for f in fs if f["type"] == "message.part.delta"]
check("收到 %d 条 delta" % len(deltas), len(deltas) == len(CHUNKS), "期望 %d" % len(CHUNKS))
check("没有 session.error", "session.error" not in types)

say("== 4. 把 delta 拼起来（界面就是这么干的）==")
part_ids = {d["data"]["partID"] for d in deltas}
check("part id 全程稳定", len(part_ids) == 1, str(part_ids))
assembled = "".join(d["data"]["delta"] for d in deltas)
check("拼出来 == 完整正文", assembled == FULL, repr(assembled))

say("== 5. 落库内容一致（刷新后不变样）==")
msgs = json.loads(get("/api/session/%s/message" % sid)[1].decode("utf-8"))
check("两条消息", len(msgs) == 2, str(len(msgs)))
if len(msgs) == 2:
    check("第一条是 user", msgs[0]["role"] == "user")
    check("第二条是 assistant", msgs[1]["role"] == "assistant")
    txt = "".join(p.get("text", "") for p in msgs[1]["parts"])
    check("落库正文 == 流式正文", txt == FULL, repr(txt))
    check("落库 part id == 事件里的", msgs[1]["parts"][0]["id"] in part_ids)

say("== 6. 标题自动从第一句话来 ==")
title = json.loads(get("/api/session/" + sid)[1].decode("utf-8"))["title"]
check("标题 = 你是谁？", title == "你是谁？", repr(title))

say("== 7. 发给模型的请求体对不对 ==")
b = json.loads(ModelHandler.last_body.decode("utf-8"))
check("model 正确", b.get("model") == "glm-5.3", repr(b.get("model")))
check("stream=true", b.get("stream") is True)
check("带了用户那句话", b["messages"][-1]["content"] == "你是谁？")

say("== 8. 第二轮带上完整历史 ==")
post("/api/session/%s/prompt" % sid, {"text": "再说一遍"})
check("第二轮结束", wait_turns(2))
b = json.loads(ModelHandler.last_body.decode("utf-8"))
check("历史 3 条", len(b["messages"]) == 3, str(len(b["messages"])))
if len(b["messages"]) == 3:
    check("含上一轮的回答", b["messages"][1]["content"] == FULL)

say("== 9. parts 形式的 body 也认 ==")
st, raw = post("/api/session/%s/prompt" % sid,
               {"parts": [{"type": "text", "text": "用 parts 发的"}]})
check("接受 parts 形式", json.loads(raw.decode("utf-8"))["messageID"].startswith("msg_"))
check("第三轮结束", wait_turns(3))
b = json.loads(ModelHandler.last_body.decode("utf-8"))
check("parts 里的文本送达了模型", b["messages"][-1]["content"] == "用 parts 发的",
      repr(b["messages"][-1]["content"]))

say("== 10. 同一会话在跑时拒绝第二条（不排队，免得用户以为消息丢了）==")
post("/api/session/%s/prompt" % sid, {"text": "慢慢答"})
busy_rejected = False
try:
    post("/api/session/%s/prompt" % sid, {"text": "插队"})
except urllib.error.HTTPError as e:
    busy_rejected = (e.code == 409)
check("409 拒绝", busy_rejected)
check("第四轮结束", wait_turns(4))

say("== 11. 空 prompt 应 400 ==")
try:
    post("/api/session/%s/prompt" % sid, {"text": ""})
    check("400", False, "居然成功了")
except urllib.error.HTTPError as e:
    check("400", e.code == 400, str(e.code))

say("== 12. 中断 ==")
sid2 = json.loads(post("/api/session", {"title": ""})[1].decode("utf-8"))["id"]
before = idle_count()
post("/api/session/%s/prompt" % sid2, {"text": "数数"})
time.sleep(0.1)  # 让它吐出一两个 chunk
st, raw = post("/api/session/%s/interrupt" % sid2, {})
check("interrupt 返回 true", json.loads(raw.decode("utf-8"))["interrupted"] is True)
check("中断后进入 idle", wait_turns(before + 1))
m2 = json.loads(get("/api/session/%s/message" % sid2)[1].decode("utf-8"))
check("半截内容也落库", len(m2) == 2 and len(m2[1]["parts"]) >= 1)

stop.set()
proc.terminate()
model.shutdown()

say("")
if fails:
    say("失败 %d 项: %s" % (len(fails), ", ".join(fails)))
    sys.exit(1)
say("M2 端到端全部通过")
