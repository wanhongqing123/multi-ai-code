#!/usr/bin/env python3
"""Exercise the video-circle C ABI against disposable TLS auth and OSS services."""

import http.server
import json
import os
import re
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit


OBJECT_ID = "123e4567-e89b-42d3-a456-426614174000"
POST_ID = "5c7e76f8-1122-4400-8899-123456789abc"
REPORT_ID = "c47e76f8-1122-4400-8899-123456789abc"
MAX_VIDEO_BYTES = 200_000_000


class VideoCircleServer(http.server.ThreadingHTTPServer):
    def __init__(self, address):
        super().__init__(address, VideoCircleHandler)
        self.lock = threading.Lock()
        self.signed = {}
        self.uploads = {}
        self.posts = {}
        self.blocks = {}
        self.reports = {}
        self.signed_puts = 0
        self.auth_on_put = 0
        self.redirect_followed = 0
        self.withdraw_cleanup_pending = False

    def handle_error(self, _request, _client_address):
        # Invalid TLS and early client aborts are intentional negative cases.
        pass


class VideoCircleHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def reply(self, code, body):
        encoded = json.dumps(body, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def body(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > 4096:
            return None
        try:
            return json.loads(self.rfile.read(length) or b"{}")
        except (json.JSONDecodeError, UnicodeDecodeError):
            return None

    def account(self):
        return {"Bearer author-token": "author", "Bearer viewer-token": "viewer"}.get(
            self.headers.get("Authorization"))

    def route_path(self):
        return urlsplit(self.path).path

    def origin(self):
        return f"https://localhost:{self.server.server_port}"

    def do_GET(self):
        path = self.route_path()
        if path == "/oss/play.mp4":
            with self.server.lock:
                content = next(iter(self.server.uploads.values()), None)
            if content is None:
                self.reply(404, {"error": "Video not found"})
                return
            self.send_response(200)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
            return
        if path == "/test/state":
            with self.server.lock:
                self.reply(200, {"signed_puts": self.server.signed_puts,
                    "auth_on_put": self.server.auth_on_put,
                    "redirect_followed": self.server.redirect_followed,
                    "upload_count": len(self.server.uploads),
                    "post_count": len(self.server.posts)})
            return
        account = self.account()
        if not account:
            self.reply(401, {"error": "Authentication required"})
            return
        if path == "/auth/v1/video-circle/feed":
            if urlsplit(self.path).query == "before=slow":
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", "8192")
                self.end_headers()
                try:
                    self.wfile.write(b" " * 1024)
                    self.wfile.flush()
                    time.sleep(0.6)
                    self.wfile.write(b" " * 7168)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    pass
                return
            with self.server.lock:
                posts = [{"id": post_id, "title": post["title"],
                    "author_id": post["author"], "duration_ms": post["duration_ms"]}
                    for post_id, post in self.server.posts.items()
                    if post["status"] == "approved" and
                    post["author"] not in self.server.blocks.get(account, set())]
            self.reply(200, {"posts": posts, "next_cursor": None})
            return
        if path == "/auth/v1/video-circle/blocks":
            with self.server.lock:
                authors = [{"author_id": author, "author_name": author}
                    for author in sorted(self.server.blocks.get(account, set()))]
            self.reply(200, {"authors": authors})
            return
        if path == "/auth/v1/video-circle/mine":
            if urlsplit(self.path).query == "before=mine-cursor":
                self.reply(200, {"posts": [], "next_cursor": None,
                    "cursor_received": "mine-cursor"})
                return
            cursor = parse_qs(urlsplit(self.path).query).get("before", ["0"])[0]
            if not cursor.isdigit():
                self.reply(400, {"error": "Invalid cursor"})
                return
            offset = int(cursor)
            with self.server.lock:
                posts = [{"id": post_id, "title": post["title"],
                    "status": post["status"], "duration_ms": post["duration_ms"],
                    "review_reason": post.get("review_reason")}
                    for post_id, post in reversed(list(self.server.posts.items()))
                    if post["author"] == account]
            self.reply(200, {"posts": posts[offset:offset + 20],
                "next_cursor": str(offset + 20) if len(posts) > offset + 20 else None})
            return
        match = re.fullmatch(
            r"/auth/v1/video-circle/posts/([0-9a-f-]{36})/play-link", path)
        if match:
            with self.server.lock:
                post = self.server.posts.get(match[1])
                permitted = post and (post["status"] == "approved" or
                    (post["status"] == "pending" and post["author"] == account))
            if permitted:
                self.reply(200, {"read_url": self.origin() + "/oss/play.mp4?expires=60",
                    "read_expires_in": 60})
            else:
                self.reply(404, {"error": "Video not found"})
            return
        self.reply(404, {"error": "Not found"})

    def do_POST(self):
        path = self.route_path()
        if path == "/test/hide":
            with self.server.lock:
                post = self.server.posts.get(POST_ID)
                if not post:
                    self.reply(404, {"error": "Video not found"})
                    return
                post["status"] = "hidden"
                post["review_reason"] = "运营下架"
            self.reply(200, {"status": "hidden"})
            return
        if path == "/test/withdraw-cleanup-pending":
            self.server.withdraw_cleanup_pending = True
            self.reply(200, {"enabled": True})
            return
        if path == "/test/seed-many":
            with self.server.lock:
                for index in range(35):
                    post_id = f"11111111-2222-4333-8444-{index:012x}"
                    self.server.posts[post_id] = {"author": "author",
                        "title": f"第 {index + 1} 个视频", "status": "approved",
                        "duration_ms": 1000}
            self.reply(200, {"count": 35})
            return
        if path == "/test/review":
            with self.server.lock:
                for post in self.server.posts.values():
                    if post["status"] == "pending":
                        post["status"] = "approved"
                        self.reply(200, {"status": "approved"})
                        return
            self.reply(404, {"error": "No pending post"})
            return
        account = self.account()
        if not account:
            self.reply(401, {"error": "Authentication required"})
            return
        body = self.body()
        if not isinstance(body, dict):
            self.reply(400, {"error": "Invalid JSON"})
            return
        if path == "/media/sign-upload":
            filename = body.get("filename")
            size = body.get("size_bytes")
            if (not isinstance(filename, str) or not filename.endswith(".mp4") or
                    not isinstance(size, int) or not 0 < size <= MAX_VIDEO_BYTES or
                    body.get("purpose") != "video_circle"):
                self.reply(400, {"error": "Invalid upload"})
                return
            key = f"video-circle/{account}/2026-10-24/{OBJECT_ID}.mp4"
            signature = "redirect" if filename == "redirect.mp4" else "local-signed-put"
            with self.server.lock:
                self.server.signed[key] = {"size": size, "account": account}
            self.reply(200, {"object_key": key,
                "upload_url": self.origin() + f"/{key}?signature={signature}",
                "upload_headers": {"Content-Type": "video/mp4",
                    "x-oss-forbid-overwrite": "true"},
                "upload_expires_in": 900})
            return
        if path == "/auth/v1/video-circle/posts":
            key = body.get("object_key")
            with self.server.lock:
                signed = self.server.signed.get(key)
                uploaded = self.server.uploads.get(key)
                valid = (signed and uploaded and signed["account"] == account and
                    len(uploaded or b"") == body.get("size_bytes") == signed["size"] and
                    set(body) == {"title", "object_key", "source_tool",
                        "source_task_id", "size_bytes", "duration_ms",
                        "ai_generated"} and
                    body.get("source_tool") == "wan_video" and
                    body.get("source_task_id") == "task-1" and
                    body.get("ai_generated") is True and
                    isinstance(body.get("duration_ms"), int) and
                    0 < body["duration_ms"] <= 60000 and
                    isinstance(body.get("title"), str) and body["title"])
                if valid:
                    self.server.posts[POST_ID] = {"author": account,
                        "title": body["title"], "status": "pending",
                        "duration_ms": body["duration_ms"]}
            if valid:
                self.reply(201, {"post_id": POST_ID, "status": "pending"})
            else:
                self.reply(400, {"error": "Invalid AI video submission"})
            return
        if path in ("/auth/v1/video-circle/blocks",
                    "/auth/v1/video-circle/blocks/remove"):
            author = body.get("author_id")
            if author not in ("author", "viewer") or author == account:
                self.reply(400, {"error": "Invalid author"})
                return
            with self.server.lock:
                if path.endswith("/remove"):
                    self.server.blocks.setdefault(account, set()).discard(author)
                    blocked = False
                else:
                    self.server.blocks.setdefault(account, set()).add(author)
                    blocked = True
            self.reply(200, {"blocked": blocked, "author_id": author})
            return
        match = re.fullmatch(
            r"/auth/v1/video-circle/posts/([0-9a-f-]{36})/report", path)
        if match:
            with self.server.lock:
                post = self.server.posts.get(match[1])
                valid = (post and post["status"] == "approved" and
                    post["author"] != account and body.get("reason") == "spam")
                if valid:
                    self.server.reports[(match[1], account)] = body["reason"]
            if valid:
                self.reply(201, {"report_id": REPORT_ID, "status": "pending"})
            else:
                self.reply(404, {"error": "Video not found"})
            return
        match = re.fullmatch(
            r"/auth/v1/video-circle/posts/([0-9a-f-]{36})/withdraw", path)
        if match:
            with self.server.lock:
                post = self.server.posts.get(match[1])
                if post and post["author"] == account:
                    post["status"] = "withdrawn"
                    pending = self.server.withdraw_cleanup_pending
                    self.reply(202 if pending else 200,
                        {"post_id": match[1], "status": "withdrawn",
                         "cleanup_pending": pending})
                    return
            self.reply(404, {"error": "Video not found"})
            return
        self.reply(404, {"error": "Not found"})

    def do_PUT(self):
        path = self.route_path()
        signature = urlsplit(self.path).query
        if signature == "signature=redirect":
            self.send_response(307)
            self.send_header("Location", self.origin() + "/redirect-followed")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return
        if path == "/redirect-followed":
            with self.server.lock:
                self.server.redirect_followed += 1
            self.reply(500, {"error": "Redirect must not be followed"})
            return
        if self.headers.get("Authorization") or self.headers.get("Cookie"):
            with self.server.lock:
                self.server.auth_on_put += 1
            self.reply(400, {"error": "Account credential sent to OSS"})
            return
        key = path.removeprefix("/")
        length = int(self.headers.get("Content-Length", "0"))
        with self.server.lock:
            signed = self.server.signed.get(key)
        if (not signed or signature != "signature=local-signed-put" or
                length != signed["size"] or
                self.headers.get("Content-Type") != "video/mp4" or
                self.headers.get("x-oss-forbid-overwrite") != "true"):
            self.reply(400, {"error": "Invalid signed upload"})
            return
        content = self.rfile.read(length)
        if len(content) != length:
            self.reply(400, {"error": "Short upload"})
            return
        with self.server.lock:
            self.server.signed_puts += 1
            self.server.uploads[key] = content
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


def generate_certificate(directory, basename):
    cert = os.path.join(directory, basename + ".pem")
    key = os.path.join(directory, basename + ".key")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
        "-sha256", "-nodes", "-days", "1", "-subj", "/CN=localhost",
        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", key, "-out", cert], stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, check=True)
    return cert, key


def main():
    if len(sys.argv) != 2:
        raise SystemExit("expected video-circle test executable")
    with tempfile.TemporaryDirectory(prefix="maichat-video-circle-test-") as directory:
        cert, key = generate_certificate(directory, "localhost")
        unrelated_cert, _ = generate_certificate(directory, "unrelated")
        server = VideoCircleServer(("127.0.0.1", 0))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            endpoint = f"https://localhost:{server.server_port}"
            result = subprocess.run([sys.argv[1], endpoint, cert, unrelated_cert,
                directory], timeout=45, check=False)
            return result.returncode
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
