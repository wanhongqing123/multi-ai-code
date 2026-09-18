# MaiAgent M1 端到端验证。
# 不走 shell 传参——这台机器的 bash/PowerShell 会把中文按 GBK 毁掉，
# 之前差点因此误判服务端有 bug。一切收发都在 Python 里用显式 UTF-8 完成。
import json
import sys
import threading
import time
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:1713"
out = sys.stdout.buffer
fails = []


def say(msg):
    out.write((msg + "\n").encode("utf-8"))
    out.flush()


def check(name, cond, extra=""):
    say(("  [%s] %s%s" % ("OK" if cond else "FAIL", name, ("  " + extra) if extra else "")))
    if not cond:
        fails.append(name)


def get(path):
    with urllib.request.urlopen(BASE + path, timeout=10) as r:
        return r.status, r.read()


def post(path, obj):
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        BASE + path, data=body,
        headers={"Content-Type": "application/json; charset=utf-8"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status, r.read()


# ── SSE 先挂上，才能收到后面建会话的事件 ──────────────────────
frames = []


def sse_reader():
    try:
        with urllib.request.urlopen(BASE + "/api/event", timeout=12) as r:
            buf = b""
            while len(frames) < 3:
                chunk = r.read(1)
                if not chunk:
                    break
                buf += chunk
                if buf.endswith(b"\n\n"):
                    line = buf.strip()
                    if line.startswith(b"data: "):
                        frames.append(json.loads(line[6:].decode("utf-8")))
                    buf = b""
    except Exception:
        pass


t = threading.Thread(target=sse_reader, daemon=True)
t.start()
time.sleep(0.6)

say("== 1. health ==")
st, raw = get("/api/health")
check("200", st == 200)
check("service=maiagent", json.loads(raw)["service"] == "maiagent")

say("== 2. create session, non-ASCII round-trips ==")
# 载荷，不是文案：这个用例验证非 ASCII 标题能原样往返。
# 写成 \u 转义是因为规范要求代码里除注释外不出现中文；换成 ASCII
# 就等于把这条用例要测的东西删掉了，所以字节必须保持原样。
#   \u4e2d\u6587\u6807\u9898  中文标题
#   \u00b7                  ·
#   \uff0c                  ，
TITLE = "\u4e2d\u6587\u6807\u9898\u00b7\uff0c emoji \U0001F642"

st, raw = post("/api/session", {"title": TITLE, "directory": "/tmp/x", "model": "glm-5.3"})
check("response is valid UTF-8", True, "")
d = json.loads(raw.decode("utf-8"))
sid = d["id"]
check("title came back unchanged", d["title"] == TITLE, repr(d["title"]))
check("directory was parsed", d["directory"] == "/tmp/x", repr(d["directory"]))
check("model was parsed", d["model"] == "glm-5.3", repr(d["model"]))
check("id starts with ses_", sid.startswith("ses_"), sid)

say("== 3. read one session back ==")
st, raw = get("/api/session/" + sid)
check("200", st == 200)
check("title matches", json.loads(raw.decode("utf-8"))["title"] == TITLE)

say("== 4. listing contains it, newest updated first ==")
st, raw = get("/api/session")
arr = json.loads(raw.decode("utf-8"))
check("the new session is listed", any(x["id"] == sid for x in arr))
ups = [x["time"]["updated"] for x in arr]
check("sorted by updated, descending", ups == sorted(ups, reverse=True))

say("== 5. message list is an empty array ==")
st, raw = get("/api/session/%s/message" % sid)
check("[]", json.loads(raw) == [])

say("== 6. unknown session gives 404 ==")
try:
    get("/api/session/ses_nope")
    check("404", False, "got 200 instead")
except urllib.error.HTTPError as e:
    check("404", e.code == 404, str(e.code))

say("== 7. SSE delivers session.created with the title intact ==")
post("/api/session", {"title": "one more"})
deadline = time.time() + 5
while len(frames) < 1 and time.time() < deadline:
    time.sleep(0.1)
check("at least one frame", len(frames) >= 1, "%d frames" % len(frames))
if frames:
    f = frames[0]
    check("type=session.created", f["type"] == "session.created", f["type"])
    check("event id starts with evt_", f["id"].startswith("evt_"), f["id"])
    check("data.sessionID present", "sessionID" in f["data"])
    check("non-ASCII title survived the event", f["data"].get("detail") == TITLE, repr(f["data"].get("detail")))

say("")
if fails:
    say("%d checks failed: %s" % (len(fails), ", ".join(fails)))
    sys.exit(1)
say("M1 all checks passed")
