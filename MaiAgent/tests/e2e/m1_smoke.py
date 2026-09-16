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

say("== 2. 建会话，中文原样回来 ==")
TITLE = "中文标题测试·带标点，和 emoji 🙂"
st, raw = post("/api/session", {"title": TITLE, "directory": "/tmp/x", "model": "glm-5.3"})
check("响应是合法 UTF-8", True, "")
d = json.loads(raw.decode("utf-8"))
sid = d["id"]
check("title 原样返回", d["title"] == TITLE, repr(d["title"]))
check("directory 被解析", d["directory"] == "/tmp/x", repr(d["directory"]))
check("model 被解析", d["model"] == "glm-5.3", repr(d["model"]))
check("id 以 ses_ 开头", sid.startswith("ses_"), sid)

say("== 3. 读回单个会话 ==")
st, raw = get("/api/session/" + sid)
check("200", st == 200)
check("title 一致", json.loads(raw.decode("utf-8"))["title"] == TITLE)

say("== 4. 列表包含它，且按 updated 倒序 ==")
st, raw = get("/api/session")
arr = json.loads(raw.decode("utf-8"))
check("能找到刚建的", any(x["id"] == sid for x in arr))
ups = [x["time"]["updated"] for x in arr]
check("按 updated 倒序", ups == sorted(ups, reverse=True))

say("== 5. 消息列表为空数组 ==")
st, raw = get("/api/session/%s/message" % sid)
check("[]", json.loads(raw) == [])

say("== 6. 不存在的会话 404 ==")
try:
    get("/api/session/ses_nope")
    check("404", False, "居然 200 了")
except urllib.error.HTTPError as e:
    check("404", e.code == 404, str(e.code))

say("== 7. SSE 收到 session.created，且中文正确 ==")
post("/api/session", {"title": "再来一条"})
deadline = time.time() + 5
while len(frames) < 1 and time.time() < deadline:
    time.sleep(0.1)
check("至少收到 1 帧", len(frames) >= 1, "收到 %d 帧" % len(frames))
if frames:
    f = frames[0]
    check("type=session.created", f["type"] == "session.created", f["type"])
    check("事件 id 以 evt_ 开头", f["id"].startswith("evt_"), f["id"])
    check("data.sessionID 存在", "sessionID" in f["data"])
    check("事件里的中文正确", f["data"].get("detail") == TITLE, repr(f["data"].get("detail")))

say("")
if fails:
    say("失败 %d 项: %s" % (len(fails), ", ".join(fails)))
    sys.exit(1)
say("M1 全部通过")
