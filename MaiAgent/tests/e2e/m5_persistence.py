# M5 端到端：落库和切模型，走真实 HTTP。
#
# 单元测试（MaiStoreTests）证明的是存储层本身对。这一份证明的是另一件事：
# **bridge 重启之后，上一次的会话和消息还在**——这才是"持久化"三个字
# 对用户的意义。做法就是最朴素的那种：起一个 bridge、写点东西、
# 把进程杀掉、指着同一个 .db 再起一个、看还在不在。
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

BRIDGE = r"E:\OpenSource\multi-ai-code\MaiAgent\build\bin\maiagent-bridge.exe"

# 载荷，不是文案：这条链路要验证非 ASCII 在 SQLite 里存进去取出来不变形。
#   \u4e2d\u6587\u6807\u9898  中文标题
#   \u4f60\u597d              你好
CJK_TITLE = "\u4e2d\u6587\u6807\u9898 \U0001F642"
CJK_TEXT = "\u4f60\u597d"


def say(message):
    out.write((message + "\n").encode("utf-8"))
    out.flush()


def check(name, condition, extra=""):
    say("  [%s] %s%s" % ("OK" if condition else "FAIL", name, ("  " + extra) if extra else ""))
    if not condition:
        fails.append(name)


# ── 假模型：回一句固定的话就行，这个文件不测流式 ────────────────
class ModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        frame = {"choices": [{"delta": {"content": CJK_TEXT}}]}
        self.wfile.write(("data: " + json.dumps(frame, ensure_ascii=False) + "\n\n")
                         .encode("utf-8"))
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


model = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
model_port = model.server_address[1]
threading.Thread(target=model.serve_forever, daemon=True).start()
say("fake model listening on 127.0.0.1:%d" % model_port)

workdir = tempfile.mkdtemp(prefix="maiagent-m5-")
# 数据库放在一层还不存在的子目录里，顺带验证父目录会被自动建出来。
database = os.path.join(workdir, "state", "agent.db")
say("database " + database)


def start_bridge():
    process = subprocess.Popen(
        [BRIDGE, "--db", database,
         "--model-url", "http://127.0.0.1:%d" % model_port,
         "--model-key", "fake", "--model", "glm-5.3"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    deadline = time.time() + 15
    while time.time() < deadline:
        line = process.stdout.readline().decode("utf-8", "replace")
        if not line:
            break
        found = re.search(r"(http://127\.0\.0\.1:\d+)", line)
        if found:
            return process, found.group(1)
    process.kill()
    return None, None


def stop_bridge(process):
    process.terminate()
    try:
        process.wait(timeout=10)
    except Exception:
        process.kill()


def get(base, path):
    with urllib.request.urlopen(base + path, timeout=10) as response:
        return response.status, response.read()


def post(base, path, payload):
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else b""
    request = urllib.request.Request(
        base + path, data=data,
        headers={"Content-Type": "application/json; charset=utf-8"})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def wait_idle(base, session_id, limit=15.0):
    deadline = time.time() + limit
    while time.time() < deadline:
        status, raw = get(base, "/api/session/%s/message" % session_id)
        messages = json.loads(raw.decode("utf-8"))
        if len(messages) >= 2:
            for part in messages[-1].get("parts", []):
                if part.get("type") == "text" and part.get("text"):
                    return messages
        time.sleep(0.05)
    return json.loads(get(base, "/api/session/%s/message" % session_id)[1].decode("utf-8"))


# ══ 第一次启动：建会话、发消息、切模型 ════════════════════════
say("")
say("== 1. first run: create, chat, switch model ==")
process, base = start_bridge()
if not base:
    say("bridge did not start")
    sys.exit(1)
say("bridge on " + base)

check("parent directory was created", os.path.isdir(os.path.dirname(database)))

status, raw = post(base, "/api/session",
                   {"title": CJK_TITLE, "directory": workdir, "model": "glm-5.3"})
session = json.loads(raw.decode("utf-8"))
session_id = session["id"]
check("session created", status == 200 and bool(session_id))
check("non-ASCII title accepted", session.get("title") == CJK_TITLE, repr(session.get("title")))

post(base, "/api/session/%s/prompt" % session_id, {"text": "hello there"})
messages = wait_idle(base, session_id)
check("two messages after the turn", len(messages) == 2, "%d" % len(messages))

# 切模型：M5 明确要的能力。只送 model，标题不能被顺手清掉。
status, raw = post(base, "/api/session/%s" % session_id, {"model": "glm-4.6"})
check("switch model returns 200", status == 200, str(status))
if status == 200:
    updated = json.loads(raw.decode("utf-8"))
    check("model changed", updated.get("model") == "glm-4.6", repr(updated.get("model")))
    check("title survived the switch", updated.get("title") == CJK_TITLE,
          repr(updated.get("title")))

status, _ = post(base, "/api/session/ses_nope", {"model": "x"})
check("unknown session gives 404", status == 404, str(status))

stop_bridge(process)
say("bridge stopped")

check("database file exists on disk", os.path.isfile(database))
check("database is not empty", os.path.getsize(database) > 0,
      "%d bytes" % (os.path.getsize(database) if os.path.isfile(database) else 0))

# ══ 第二次启动：同一个库，东西必须还在 ════════════════════════
say("")
say("== 2. second run: same database, everything still there ==")
process, base = start_bridge()
if not base:
    say("bridge did not restart")
    sys.exit(1)
say("bridge on " + base)

status, raw = get(base, "/api/session")
sessions = json.loads(raw.decode("utf-8"))
check("session list survived the restart", len(sessions) == 1, "%d" % len(sessions))

status, raw = get(base, "/api/session/" + session_id)
check("session can be read back", status == 200, str(status))
if status == 200:
    reloaded = json.loads(raw.decode("utf-8"))
    check("title round-tripped through SQLite", reloaded.get("title") == CJK_TITLE,
          repr(reloaded.get("title")))
    check("switched model was persisted", reloaded.get("model") == "glm-4.6",
          repr(reloaded.get("model")))
    check("directory was persisted", reloaded.get("directory") == workdir)

status, raw = get(base, "/api/session/%s/message" % session_id)
messages = json.loads(raw.decode("utf-8"))
check("messages survived the restart", len(messages) == 2, "%d" % len(messages))
if len(messages) == 2:
    check("first is the user prompt", messages[0].get("role") == "user")
    check("second is the assistant reply", messages[1].get("role") == "assistant")
    texts = [p.get("text") for m in messages for p in m.get("parts", [])
             if p.get("type") == "text"]
    check("user text intact", "hello there" in texts, repr(texts))
    check("non-ASCII reply intact", CJK_TEXT in texts, repr(texts))

# 第三轮对话接着上一次的历史跑——重启之后上下文没丢，才算真的接上了
post(base, "/api/session/%s/prompt" % session_id, {"text": "still with me?"})
deadline = time.time() + 15
while time.time() < deadline:
    messages = json.loads(get(base, "/api/session/%s/message" % session_id)[1].decode("utf-8"))
    if len(messages) >= 4:
        break
    time.sleep(0.05)
check("can keep chatting after the restart", len(messages) == 4, "%d" % len(messages))

# 删掉会话：消息要跟着走，别留孤儿
status, _ = post(base, "/api/session/%s/interrupt" % session_id, {})
stop_bridge(process)

# ══ 收尾 ══════════════════════════════════════════════════════
model.shutdown()
shutil.rmtree(workdir, ignore_errors=True)

say("")
say("-" * 40)
if fails:
    say("M5 end-to-end: %d checks failed:" % len(fails))
    for name in fails:
        say("  - " + name)
    sys.exit(1)
say("M5 end-to-end: all checks passed")
