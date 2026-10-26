#!/usr/bin/env python3
"""Run the Desktop cloud dialog against the shared disposable TLS auth/OSS fixture."""

import os
import ssl
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "../../Network/tests")))
from run_cloud_drive_local_tls import Fixture, Handler


class DesktopHandler(Handler):
    def do_GET(self):
        if self.path_only() == "/oss/thumbnail":
            try:
                with open(self.server.thumbnail_path, "rb") as source:
                    thumbnail = source.read()
            except OSError:
                self.reply(404, {"error": "thumbnail_missing"})
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(thumbnail)))
            self.end_headers()
            self.wfile.write(thumbnail)
            return
        super().do_GET()


def main():
    if len(sys.argv) != 3:
        raise SystemExit("expected Desktop cloud test executable and fixture MP4")
    with tempfile.TemporaryDirectory(prefix="maichat-desktop-cloud-") as directory:
        cert = os.path.join(directory, "server.pem")
        key = os.path.join(directory, "server.key")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
            "-sha256", "-nodes", "-days", "1", "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-keyout", key, "-out", cert], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, check=True)
        server = Fixture(("127.0.0.1", 0))
        server.slow_complete = False
        server.RequestHandlerClass = DesktopHandler
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            env = os.environ.copy()
            env["QT_QPA_PLATFORM"] = "offscreen"
            screenshots = env.get("MAICHAT_CLOUD_UI_SCREENSHOTS", directory)
            os.makedirs(screenshots, exist_ok=True)
            server.thumbnail_path = os.path.join(screenshots, "cloud-test-image.jpg")
            return subprocess.run([sys.argv[1],
                f"https://localhost:{server.server_port}", cert, sys.argv[2],
                screenshots], env=env, timeout=90, check=False).returncode
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
