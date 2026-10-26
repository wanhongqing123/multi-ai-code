#!/usr/bin/env python3
"""Local TLS transfer fixture; creates its own certificate and private key."""

import glob
import http.server
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/ok?token=a%2Fb&scope=x%20y")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/insecure":
            self.send_response(302)
            self.send_header("Location", "http://localhost/ok")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/bad":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/header-large":
            self.send_response(200)
            self.send_header("Content-Length", "1000000")
            self.end_headers()
            return
        if self.path == "/empty":
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/slow":
            open(os.path.join(self.server.output_dir, "slow-started"), "wb").close()
            self.send_response(200)
            self.send_header("Content-Length", "1024")
            self.end_headers()
            for _ in range(64):
                try:
                    self.wfile.write(b"x" * 16)
                    self.wfile.flush()
                    time.sleep(0.05)
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    break
            return
        body = b"x" * 128 if self.path == "/large" else self.path.encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("expected test executable")
    with tempfile.TemporaryDirectory(prefix="maichat-curl-test-") as directory:
        cert = os.path.join(directory, "localhost.pem")
        key = os.path.join(directory, "localhost.key")
        unrelated_cert = os.path.join(directory, "unrelated.pem")
        for output_cert, output_key in (
            (cert, key), (unrelated_cert, os.path.join(directory, "unrelated.key"))
        ):
            subprocess.run(
                ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256",
                 "-nodes", "-days", "1", "-subj", "/CN=localhost",
                 "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                 "-keyout", output_key, "-out", output_cert],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        class QuietServer(http.server.ThreadingHTTPServer):
            def handle_error(self, _request, _client_address):
                # The client deliberately cuts off oversize and canceled streams.
                pass

        server = QuietServer(("127.0.0.1", 0), Handler)
        server.output_dir = directory
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            endpoint = f"https://localhost:{server.server_port}"
            completed = subprocess.run([sys.argv[1], endpoint, cert, unrelated_cert, directory],
                                       timeout=30, check=False)
            leftovers = glob.glob(os.path.join(directory, "*.maitmp-*"))
            if leftovers:
                raise RuntimeError(f"partial temp files remain: {leftovers}")
            return completed.returncode
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
