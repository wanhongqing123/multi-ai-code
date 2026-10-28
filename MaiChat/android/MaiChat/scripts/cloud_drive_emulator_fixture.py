#!/usr/bin/env python3
"""Isolated Android cloud-drive TLS fixture; never uses production OSS.

Run beside `services/auth/session-sync-ui-fixture.js` on the Mac. The latter
issues two local test-account tokens; this fixture validates them through its
local auth/me route and reuses the shared C++ client's fake multipart OSS.
"""

import http.server
import importlib.util
import json
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
from urllib.parse import parse_qs, urlsplit


SOURCE = Path(__file__).resolve().parents[3] / "Network/tests/run_cloud_drive_local_tls.py"
SPEC = importlib.util.spec_from_file_location("maichat_cloud_fixture_core", SOURCE)
CORE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CORE)


class CloudHandler(CORE.Handler):
    def reply(self, status, body):
        # Match the actual account service: timestamps are millisecond integers.
        if isinstance(body, dict):
            if "read_url" in body:
                body["size_bytes"] = self.server.size
                body["content_type"] = self.server.content_type
                body["media_type"] = self.server.media_type()
            if isinstance(body.get("created_at"), str):
                body["created_at"] = 1_761_280_000_000
            for video in body.get("videos", []):
                if isinstance(video.get("created_at"), str):
                    video["created_at"] = 1_761_280_000_000
            if isinstance(body.get("quota"), dict):
                body["quota"]["limit_bytes"] = 10_000_000_000
        super().reply(status, body)

    def end_headers(self):
        if self.command == "PUT" and \
                parse_qs(urlsplit(self.path).query).get("partNumber") == ["1"]:
            # First part is already stored but the client must wait for OSS's
            # response; this leaves time to exercise a real pause before part 2.
            time.sleep(3.0)
        super().end_headers()

    def path_only(self):
        path = super().path_only()
        return "/auth" + path if path.startswith("/v1/cloud-drive/") else path

    def account(self):
        bearer = self.headers.get("Authorization", "")
        if not bearer.startswith("Bearer "):
            return None
        try:
            request = urllib.request.Request(
                "http://127.0.0.1:18190/v1/auth/me",
                headers={"Authorization": bearer})
            with urllib.request.urlopen(request, timeout=4) as response:
                identity = json.load(response)["user"]["id"]
            return {"sync-ui-test": "author", "sync-ui-viewer": "viewer"}.get(identity)
        except (KeyError, urllib.error.URLError, TimeoutError, ValueError):
            return None

    def do_GET(self):
        if self.path_only() == "/test/health":
            self.reply(200, {"fixture": True, "tls": True})
            return
        if self.path_only() == "/oss/thumbnail":
            if parse_qs(urlsplit(self.path).query).get("signature") != ["fixture"]:
                self.reply(403, {"error": "invalid_signature"})
                return
            data = self.server.thumbnail
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        super().do_GET()

    def do_PUT(self):
        if parse_qs(urlsplit(self.path).query).get("partNumber") == ["2"]:
            time.sleep(1.0)
        super().do_PUT()


class Control(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.path == "/test/cert":
            data = self.server.certificate
            self.send_response(200)
            self.send_header("Content-Type", "application/x-pem-file")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if self.path == "/test/state":
            fixture = self.server.cloud
            with fixture.lock:
                body = {"sessions": fixture.sessions,
                        "parts": sorted(fixture.part_bytes),
                        "status": fixture.status,
                        "size": fixture.size,
                        "owner": "sync-ui-test",
                        "oss_authorization": fixture.oss_authorization}
                body["range_requests"] = fixture.range_requests
                body["full_downloads"] = fixture.full_downloads
            data = json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        self.send_error(404)

    def do_POST(self):
        if self.path != "/test/reset":
            self.send_error(404)
            return
        fixture = self.server.cloud
        with fixture.lock:
            fixture.part_bytes.clear()
            fixture.signs.clear()
            fixture.sessions = 0
            fixture.oss_authorization = 0
            fixture.range_requests = 0
            fixture.full_downloads = 0
            fixture.status = "none"
            fixture.name = ""
            fixture.content_type = ""
            fixture.size = 0
            fixture.folders.clear()
            fixture.folder_id = None
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()


def main():
    with tempfile.TemporaryDirectory(prefix="maichat-android-cloud-") as directory:
        cert = os.path.join(directory, "localhost.pem")
        key = os.path.join(directory, "localhost.key")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256",
            "-nodes", "-days", "1", "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-keyout", key, "-out", cert], check=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(cert, "rb") as source:
            certificate = source.read()
        cloud = CORE.Fixture(("127.0.0.1", 18446))
        cloud.RequestHandlerClass = CloudHandler
        cloud.thumbnail = (Path(__file__).resolve().parents[1] / "app/src/androidTest/assets" /
            "cloud-drive-thumbnail.jpg").read_bytes()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        cloud.socket = context.wrap_socket(cloud.socket, server_side=True)
        control = http.server.ThreadingHTTPServer(("127.0.0.1", 18447), Control)
        control.certificate = certificate
        control.cloud = cloud
        thread = threading.Thread(target=control.serve_forever, daemon=True)
        thread.start()
        print("cloud fixture: https://localhost:18446 control: http://127.0.0.1:18447",
              flush=True)
        try:
            cloud.serve_forever()
        finally:
            cloud.server_close()
            control.shutdown()
            control.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    main()
