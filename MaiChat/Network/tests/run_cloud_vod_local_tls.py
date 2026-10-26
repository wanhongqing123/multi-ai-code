#!/usr/bin/env python3
"""Exercise bounded HTTPS Range VOD against a disposable local TLS server."""

import http.server
import re
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit


VIDEO_ID = "123e4567-e89b-42d3-a456-426614174000"
RENEW_ID = "223e4567-e89b-42d3-a456-426614174000"
VIRTUAL_SIZE = 1_900_000_001
PAYLOAD = (bytes(range(251)) * ((12 * 1024 * 1024 + 123) // 251 + 1))[:
    12 * 1024 * 1024 + 123]


class Fixture(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("localhost", 0), Handler)
        self.range_calls = 0
        self.bytes_served = 0
        self.max_range_first = 0
        self.range_start_counts = {}
        self.oss_connections = set()
        self.oss_auth_headers = 0
        self.auth_calls = 0
        self.renew_auth_calls = 0
        self.lock = threading.Lock()
        self.payload = PAYLOAD

    def handle_error(self, *_args):
        # Range-ignored and cancellation tests intentionally close connections.
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def reply(self, status, body, kind="application/json"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/metrics":
            with self.server.lock:
                result = (self.server.range_calls, self.server.bytes_served,
                          self.server.auth_calls, self.server.max_range_first,
                          self.server.renew_auth_calls,
                          len(self.server.oss_connections),
                          self.server.oss_auth_headers)
            self.reply(200, "%d %d %d %d %d %d %d" % result, "text/plain")
            return
        if path in (f"/auth/v1/cloud-drive/videos/{VIDEO_ID}/play-link",
                    f"/auth/v1/cloud-drive/videos/{RENEW_ID}/play-link"):
            if self.headers.get("Authorization") != "Bearer local-token":
                self.reply(401, '{"error":"auth_required"}')
                return
            with self.server.lock:
                if RENEW_ID in path:
                    self.server.renew_auth_calls += 1
                    calls = self.server.renew_auth_calls
                else:
                    self.server.auth_calls += 1
                    calls = self.server.auth_calls
            route = "expire-after-first" if calls == 1 or (RENEW_ID in path and calls == 2) else "play"
            body = ('{"read_url":"https://localhost:%d/oss/%s?sig=valid",'
                    '"read_expires_in":60,"size_bytes":%d,'
                    '"content_type":"video/quicktime","media_type":"video"}'
                    % (self.server.server_port, route, len(self.server.payload)))
            if RENEW_ID in path and calls == 2:
                payload = body.encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                try:
                    self.wfile.write(payload[:len(payload) // 2])
                    self.wfile.flush()
                    time.sleep(2)
                    self.wfile.write(payload[len(payload) // 2:])
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    pass
            else:
                self.reply(200, body)
            return
        if not path.startswith("/oss/"):
            self.reply(404, "missing", "text/plain")
            return
        with self.server.lock:
            self.server.oss_connections.add(self.client_address[1])
            if self.headers.get("Authorization") or self.headers.get("Proxy-Authorization"):
                self.server.oss_auth_headers += 1
        if path == "/oss/expired":
            self.reply(403, "expired", "text/plain")
            return
        if path == "/oss/ignore-range":
            self.reply(200, self.server.payload, "video/quicktime")
            return
        match = re.fullmatch(r"bytes=(\d+)-(\d+)",
                             self.headers.get("Range", ""))
        if not match:
            self.reply(400, "range required", "text/plain")
            return
        first, requested_last = map(int, match.groups())
        if path == "/oss/expire-after-first" and first:
            self.reply(403, "expired", "text/plain")
            return
        object_size = (VIRTUAL_SIZE if path == "/oss/large-virtual" else
                       1683 if path == "/oss/reject-beyond" else
                       len(self.server.payload))
        if path == "/oss/reject-beyond" and requested_last >= object_size:
            self.reply(200, self.server.payload[:object_size], "video/quicktime")
            return
        if first >= object_size or requested_last < first:
            # OSS has been seen returning 200/full body here. The client must
            # never accept or cache that response as a valid range.
            self.reply(200, self.server.payload, "video/quicktime")
            return
        last = min(requested_last, object_size - 1)
        payload = (bytes(range(251)) * ((last - first + 1) // 251 + 2))[
            first % 251:first % 251 + (last - first + 1)] if path == "/oss/large-virtual" else self.server.payload[first:last + 1]
        with self.server.lock:
            self.server.range_calls += 1
            self.server.max_range_first = max(self.server.max_range_first, first)
            range_key = (path, first)
            self.server.range_start_counts[range_key] = (
                self.server.range_start_counts.get(range_key, 0) + 1)
            same_range_calls = self.server.range_start_counts[range_key]
        self.send_response(206)
        total = object_size if path != "/oss/oversize" else 2_000_000_001
        self.send_header("Content-Range", f"bytes {first}-{last}/{total}")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Content-Type", "video/quicktime")
        etag = '"changed"' if path == "/oss/etag-drift" and first else '"original"'
        self.send_header("ETag", etag)
        self.end_headers()
        try:
            if first and (path == "/oss/slow" or
                          (path == "/oss/slow-once" and same_range_calls == 1)):
                for start in range(0, len(payload), 8192):
                    part = payload[start:start + 8192]
                    self.wfile.write(part)
                    self.wfile.flush()
                    with self.server.lock:
                        self.server.bytes_served += len(part)
                    time.sleep(0.02)
            else:
                self.wfile.write(payload)
                with self.server.lock:
                    self.server.bytes_served += len(payload)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


def main():
    if len(sys.argv) != 2:
        raise SystemExit("expected C++ runner path")
    with tempfile.TemporaryDirectory(prefix="maichat-vod-tls-") as directory:
        directory = Path(directory)
        cert = directory / "cert.pem"
        key = directory / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                        "-nodes", "-days", "1", "-keyout", str(key),
                        "-out", str(cert), "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost"],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        server = Fixture()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(cert), str(key))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            subprocess.run([sys.argv[1],
                            f"https://localhost:{server.server_port}",
                            str(cert), str(len(PAYLOAD))],
                           check=True, timeout=50)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    main()
