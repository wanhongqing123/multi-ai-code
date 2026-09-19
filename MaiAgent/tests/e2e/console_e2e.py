# 端到端：把整个产物当交互程序用一遍。
#
# 这是**唯一**的端到端脚本。以前还有四个（m1/m2/m4/m5），验的是"经过 REST + SSE
# 之后行为还对不对"——它们测的是 Electron 那套协议，协议和界面一起撤掉了，
# 脚本跟着撤。剩下的这一份直接对着产物：maiagent-console 只链 maiagent，
# 进程内订阅事件总线，全程没有 socket 服务端、没有 REST、没有 SSE。
#
# 两件事分开验：
#
#   1. 静态：产物的 exe 里翻不到 httplib。
#   2. 动态：聊天、工具、授权、切会话、落库，全部通过 stdin / stdout。
#      能用才算数，编译过不算。
#
# 唯一还剩的 HTTP 是**客户端**那一侧：核心用 libcurl 调模型。
# 这个脚本起的假模型就是个真的 Chat Completions 端点，那条线本来就该在。
#
# 一切收发都在 Python 里显式 UTF-8，不经过 shell 传参——
# 这台机器的 bash/PowerShell 会把中文按 GBK 毁掉，之前两次误判服务端有 bug。
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

out = sys.stdout.buffer
fails = []

CONSOLE = r"E:\OpenSource\multi-ai-code\MaiAgent\build\bin\maiagent-console.exe"


def say(m):
    out.write((m + "\n").encode("utf-8"))
    out.flush()


def check(name, cond, extra=""):
    say("  [%s] %s%s" % ("OK" if cond else "FAIL", name, ("  " + extra) if extra else ""))
    if not cond:
        fails.append(name)


# ── 1. 静态：产物里没有 HTTP 服务端 ──────────────────────────────
say("== 1. the shipped binary carries no HTTP server ==")

with open(CONSOLE, "rb") as f:
    binary = f.read()
# 对照组是 MaiModelClientTests：它**故意**链了 httplib，拿来当假的模型服务端。
# 只说"console 里没有"是不够的——万一这个标记本来就编不进任何 exe，
# 那这条检查恒为真，等于什么都没验（规范第 13 节）。
# 顺带也把话说清楚了：httplib 在这个仓库里只剩测试夹具这一个身份。
fixture = CONSOLE.replace("maiagent-console", "MaiModelClientTests")
with open(fixture, "rb") as f:
    fixture_binary = f.read()

check("the product has no httplib", binary.count(b"httplib") == 0)
check("but the test fixture does (so the probe works)", fixture_binary.count(b"httplib") > 0,
      "%d hits" % fixture_binary.count(b"httplib"))
# 适配器那几条路由。别写成 b"/api/" —— 帮助文本里的 GLM 地址
# （.../api/paas/v4）也会撞上，那是**客户端**要调的地址，不是路由。
for route in (b"/api/session", b"/api/event", b"/api/permission"):
    check("no " + route.decode() + " route", binary.count(route) == 0)

# 反过来，客户端那一侧必须还在：核心得能调模型。
check("client-side SSE is still there", binary.count(b"text/event-stream") > 0)
say("")


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
        text, tool, args = SCRIPTS[idx] if idx < len(SCRIPTS) else ("", "", "")

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
            # 一个字一个字吐，顺便验多字节字符被切在 chunk 边界上还能拼回来。
            for piece in text:
                frame = {"choices": [{"delta": {"content": piece}}]}
                self.wfile.write(("data: " + json.dumps(frame, ensure_ascii=False) + "\n\n")
                                 .encode("utf-8"))
                self.wfile.flush()
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


ModelHandler.bodies = []
model = ThreadingHTTPServer(("127.0.0.1", 0), ModelHandler)
model_port = model.server_address[1]
threading.Thread(target=model.serve_forever, daemon=True).start()

workspace = tempfile.mkdtemp(prefix="maiagent-m6-")
database = os.path.join(workspace, "agent.db")

# 载荷，不是文案：合起来是 "你好！我是跑在 C++ 核心里的测试模型 🙂"
GREETING = ("\u4f60\u597d\uff01\u6211\u662f\u8dd1\u5728 C++ "
            "\u6838\u5fc3\u91cc\u7684\u6d4b\u8bd5\u6a21\u578b \U0001F642")

SCRIPTS[:] = [
    (GREETING, "", ""),                                                    # 第一轮：纯聊天
    ("", "write", json.dumps({"path": "note.txt", "content": "written after approval"})),
    ("Done, I wrote note.txt.", "", ""),                                   # 第二轮的收尾
]

say("== 2. drive the product as an interactive program ==")
say("  workspace " + workspace)
say("  fake model on 127.0.0.1:%d" % model_port)

proc = subprocess.Popen(
    [CONSOLE,
     "--dir", workspace,
     "--db", database,
     "--model-url", "http://127.0.0.1:%d" % model_port,
     "--model-key", "fake", "--model", "glm-5.3"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

# stdout 是管道，所以控制台那边走的是"原样写 UTF-8 字节"那条路
# （WriteConsoleW 对管道无效，见 MaiConsoleMain.cpp 里 writeUtf8 的注释）。
screen = []
screen_lock = threading.Lock()


def pump():
    while True:
        chunk = proc.stdout.read(1)
        if not chunk:
            return
        with screen_lock:
            screen.append(chunk)


threading.Thread(target=pump, daemon=True).start()


def text():
    with screen_lock:
        return b"".join(screen).decode("utf-8", "replace")


def wait_for(needle, timeout=20):
    """等屏幕上出现某段文字。只能用在"这段文字以前没出现过"的地方。"""
    return wait_after(needle, 0, timeout)


def wait_after(needle, mark, timeout=20):
    """等屏幕上 mark 之后出现某段文字。

    验一条命令的输出必须用这个，不能用 wait_for：屏幕上早就有过同样的字了
    （工具卡在流式输出里就打过一遍），那样的检查会立刻为真——其实命令的输出
    根本还没打出来，等于什么都没等到。这就是规范第 13 节说的
    "这套观测能不能看见目标"，这里差点踩进去。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in text()[mark:]:
            return True
        time.sleep(0.05)
    return False


def send(line):
    proc.stdin.write((line + "\n").encode("utf-8"))
    proc.stdin.flush()


# ── 开场 ────────────────────────────────────────────────────
check("banner says it links the library only", wait_for("links maiagent only"))
check("banner shows the session id", "ses_" in text())
check("banner shows the database", database in text())

# ── 第一轮：纯聊天，验流式 ──────────────────────────────────
send("\u4f60\u597d")
check("streams the full multi-byte answer", wait_for(GREETING), repr(text()[-120:]))

# ── 第二轮：工具 + 授权 ────────────────────────────────────
mark = len(text())
send("write note.txt")
check("tool card shows up as pending", wait_for("[tool] write  pending"))
check("tool card carries the arguments", "note.txt" in text()[mark:])
check("console says how to answer", wait_for("/y approve"))
check("the file is not there yet", not os.path.exists(os.path.join(workspace, "note.txt")))

mark = len(text())
send("/y")
check("approval is echoed", wait_after("approved", mark))
check("tool runs to completion", wait_after("[tool] write  completed", mark))
check("model gets the last word", wait_after("Done, I wrote note.txt.", mark))

note = os.path.join(workspace, "note.txt")
check("the file really got written", os.path.exists(note))
if os.path.exists(note):
    with open(note, "rb") as f:
        check("with the right content", f.read() == b"written after approval")

# 回灌给模型的那一轮里必须带上 tool 结果，否则它是在凭空接话。
with served_lock:
    bodies = list(ModelHandler.bodies)
check("three model calls happened", len(bodies) == 3, "%d" % len(bodies))
if len(bodies) == 3:
    fed = json.loads(bodies[2].decode("utf-8"))
    roles = [m.get("role") for m in fed.get("messages", [])]
    check("the tool result was fed back", "tool" in roles, repr(roles))

# ── 查询类命令 ────────────────────────────────────────────
mark = len(text())
send("/sessions")
check("/sessions lists the current one", wait_after("ses_", mark))

mark = len(text())
send("/history")
check("/history shows the user's words", wait_after("user", mark))
check("/history shows the tool call", wait_after("[tool] write", mark))

mark = len(text())
send("/help")
check("/help lists the commands", wait_after("/interrupt", mark))

mark = len(text())
send("/nonsense")
check("unknown commands are rejected, not sent to the model",
      wait_after("unknown command /nonsense", mark))

# ── 新建会话 ──────────────────────────────────────────────
mark = len(text())
send("/new")
check("/new reports a second session", wait_after("new session ses_", mark))

# ── 退出 ──────────────────────────────────────────────────
send("/quit")
try:
    code = proc.wait(timeout=20)
except subprocess.TimeoutExpired:
    proc.kill()
    code = -1
check("/quit exits cleanly", code == 0, "exit=%d" % code)
say("")


# ── 3. 落库了吗 ────────────────────────────────────────────
# 控制台进程已经退了，数据库里应该留着两个会话和第一个会话的全部消息。
# 用另一个进程去读——这里就借 maiagent-console 自己：同一个库，同一个文件。
say("== 3. what the console wrote is still there ==")
check("database file exists", os.path.exists(database))

reread = subprocess.run(
    [CONSOLE, "--dir", workspace, "--db", database],
    input="/sessions\n/quit\n".encode("utf-8"),
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
reopened = reread.stdout.decode("utf-8", "replace")
# 重开一次会再建一个会话（控制台一启动就建），所以至少三个。
check("the old sessions came back", reopened.count("ses_") >= 3,
      "%d mentions" % reopened.count("ses_"))

say("")


# ── 4. 硬杀之后数据还在吗 ──────────────────────────────────
# 上面那一段是**正常退出**：析构跑过、SQLite 干净关闭。真实世界不长这样——
# 用户直接叉掉窗口、进程被杀、机器断电。
#
# 这一段用 terminate() 硬杀，一个析构函数都不跑，然后重开看数据还在不在。
# 验的是 WAL：提交过的数据先落在 <db>-wal 里，下次打开时由 SQLite 自己重放。
# （顺带说明为什么只拷 agent.db 不算备份——wal 里那部分会丢。）
say("== 4. survives a hard kill ==")

workspace2 = tempfile.mkdtemp(prefix="maiagent-kill-")
database2 = os.path.join(workspace2, "agent.db")
served["n"] = 0
SURVIVOR = "this line must survive a hard kill"
SCRIPTS[:] = [(SURVIVOR, "", "")]

victim = subprocess.Popen(
    [CONSOLE, "--dir", workspace2, "--db", database2,
     "--model-url", "http://127.0.0.1:%d" % model_port, "--model-key", "fake"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

seen = []


def pump_victim():
    while True:
        chunk = victim.stdout.read(1)
        if not chunk:
            return
        seen.append(chunk)


threading.Thread(target=pump_victim, daemon=True).start()
victim.stdin.write(b"remember this\n")
victim.stdin.flush()

deadline = time.time() + 20
while time.time() < deadline:
    if SURVIVOR in b"".join(seen).decode("utf-8", "replace"):
        break
    time.sleep(0.05)
victim_screen = b"".join(seen).decode("utf-8", "replace")
check("the answer came through before the kill", SURVIVOR in victim_screen)

old_session = ""
for token in victim_screen.split():
    if token.startswith("ses_"):
        old_session = token
        break
check("captured the session id", old_session.startswith("ses_"), old_session)

# 硬杀：没有 /quit，没有析构，没有干净关库。
victim.terminate()
victim.wait(timeout=10)

revived = subprocess.run(
    [CONSOLE, "--dir", workspace2, "--db", database2],
    input=("/use %s\n/history\n/quit\n" % old_session).encode("utf-8"),
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
revived_screen = revived.stdout.decode("utf-8", "replace")
check("the killed session came back", ("now talking to " + old_session) in revived_screen)
check("and its answer is still in the history", SURVIVOR in revived_screen,
      repr(revived_screen[-200:]))

shutil.rmtree(workspace, ignore_errors=True)
shutil.rmtree(workspace2, ignore_errors=True)

say("")
say("-" * 40)
if fails:
    say("console end-to-end: %d checks FAILED" % len(fails))
    for f in fails:
        say("  - " + f)
    sys.exit(1)
say("console end-to-end: all checks passed")
