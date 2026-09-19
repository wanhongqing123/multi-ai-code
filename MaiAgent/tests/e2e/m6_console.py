# M6 端到端：脱壳验证。
#
# 前面五个脚本验的都是"经过 HTTP 之后行为还对不对"。这一份反过来，验的是
# **HTTP 可以整块摘掉**：maiagent-console 只链 maiagent 这个库，进程内直接订阅事件总线，
# 全程没有 socket 服务端、没有 REST、没有 SSE。
#
# 两件事分开验：
#
#   1. 静态：exe 里翻不到 httplib，也翻不到适配器那几条路由。
#      "adapters/ 和 cli/ 是可摘的壳"这句话到这里才算有证据。
#   2. 动态：把它当交互程序用一遍——聊天、工具、授权、切会话、落库，
#      全部通过 stdin / stdout。能用才算摘得干净，编译过不算。
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


# ── 1. 静态：这个 exe 里没有 HTTP 服务端 ─────────────────────────
say("== 1. the binary carries no HTTP server ==")

with open(CONSOLE, "rb") as f:
    binary = f.read()
bridge_path = CONSOLE.replace("maiagent-console", "maiagent-bridge")
with open(bridge_path, "rb") as f:
    bridge_binary = f.read()

# 拿 bridge 当对照组。只说"console 里没有"是不够的——万一这几个标记本来就
# 编不进任何 exe，那这条检查恒为真，等于什么都没验（规范第 13 节）。
for marker in (b"httplib", b"/api/session", b"/api/event"):
    name = marker.decode()
    check("console has no " + name, binary.count(marker) == 0)
    check("but bridge does (so the probe works)", bridge_binary.count(marker) > 0,
          "%d hits" % bridge_binary.count(marker))

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

say("== 2. drive it as an interactive program ==")
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

shutil.rmtree(workspace, ignore_errors=True)

say("")
say("-" * 40)
if fails:
    say("M6 end-to-end: %d checks FAILED" % len(fails))
    for f in fails:
        say("  - " + f)
    sys.exit(1)
say("M6 end-to-end: all checks passed")
