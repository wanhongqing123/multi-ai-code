#!/usr/bin/env python3
"""Disposable iOS cloud drive auth/OSS TLS fixture; never uses production OSS."""

import json
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit


ANDROID_SCRIPTS = Path(__file__).resolve().parents[3] / "android/MaiChat/scripts"
sys.path.insert(0, str(ANDROID_SCRIPTS))
from cloud_drive_emulator_fixture import CORE, CloudHandler, Control  # noqa: E402


class IOSCloudHandler(CloudHandler):
    def do_PUT(self):
        if parse_qs(urlsplit(self.path).query).get("partNumber") == ["2"]:
            time.sleep(2)
        super().do_PUT()

    def do_GET(self):
        if self.path_only() == "/auth/v1/cloud-drive/videos":
            disabled = os.environ.get("MAICHAT_IOS_CLOUD_DISABLED")
            if disabled in ("404", "503", "1"):
                self.reply(503 if disabled == "503" else 404,
                           {"error": "feature_disabled"})
                return
            account = self.account()
            if not account:
                self.reply(401, {"error": "auth_required"})
                return
            with self.server.lock:
                ready = self.server.status == "ready" and account == "author"
                video = {"id": CORE.VIDEO_ID, "name": self.server.name,
                         "size_bytes": self.server.size,
                         "content_type": self.server.content_type,
                         "media_type": self.server.media_type(),
                         "created_at": 1_761_300_000_000}
                reserved = (self.server.size if self.server.status == "uploading"
                            and account == "author" else 0)
                used = self.server.size if ready else 0
            self.reply(200, {"videos": [video] if ready else [],
                "next_cursor": None,
                "quota": {"used_bytes": used, "reserved_bytes": reserved,
                          "limit_bytes": 10_000_000_000}})
            return
        super().do_GET()


def main():
    with tempfile.TemporaryDirectory(prefix="maichat-ios-cloud-") as directory:
        cert = os.path.join(directory, "localhost.pem")
        key = os.path.join(directory, "localhost.key")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-sha256", "-nodes", "-days", "1", "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-keyout", key, "-out", cert], check=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(cert, "rb") as source:
            certificate = source.read()
        cloud = CORE.Fixture(("127.0.0.1", 18448))
        cloud.RequestHandlerClass = IOSCloudHandler
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        cloud.socket = context.wrap_socket(cloud.socket, server_side=True)
        control = http.server.ThreadingHTTPServer(("127.0.0.1", 18449), Control)
        control.certificate = certificate
        control.cloud = cloud
        thread = threading.Thread(target=control.serve_forever, daemon=True)
        thread.start()
        print("iOS cloud fixture: https://localhost:18448 control: http://127.0.0.1:18449",
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
