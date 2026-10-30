#!/usr/bin/env python3
"""Run a loopback HTTPS DNS-over-HTTPS fixture; it never accesses account data."""
import http.server
import os
import ssl
import struct
import subprocess
import sys
import tempfile
import threading

class Handler(http.server.BaseHTTPRequestHandler):
    dns_count = 0
    health_count = 0
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path != '/api/v1/auth/me' or self.headers.get('Host', '').split(':')[0] != 'ichat.life':
            self.send_error(404)
            return
        assert not self.headers.get('Authorization')
        Handler.health_count += 1
        self.send_response(401)
        self.send_header('Content-Length', '2')
        self.end_headers()
        self.wfile.write(b'{}')

    def do_POST(self):
        if self.path != '/dns-query':
            self.send_error(404)
            return
        Handler.dns_count += 1
        query = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        if len(query) < 17:
            self.send_error(400)
            return
        offset = 12
        while offset < len(query) and query[offset]:
            offset += query[offset] + 1
        offset += 1
        if offset + 4 > len(query):
            self.send_error(400)
            return
        qtype = struct.unpack('!H', query[offset:offset + 2])[0]
        question = query[12:offset + 4]
        answer = b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 30, 4) + b'\x7f\x00\x00\x01' if qtype == 1 else b''
        response = query[:2] + struct.pack('!HHHHH', 0x8180, 1, bool(answer), 0, 0) + question + answer
        self.send_response(200)
        self.send_header('Content-Type', 'application/dns-message')
        self.send_header('Content-Length', str(len(response)))
        self.end_headers()
        self.wfile.write(response)

with tempfile.TemporaryDirectory(prefix='maichat-auth-tls-') as root:
    def certificate(name, sans):
        key = os.path.join(root, name + '.key')
        cert = os.path.join(root, name + '.pem')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=' + name, '-addext', 'subjectAltName=' + sans,
                        '-keyout', key, '-out', cert], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        return key, cert
    key, trusted = certificate('local-fixture', 'DNS:localhost,DNS:ichat.life')
    _, wrong = certificate('wrong-fixture', 'DNS:untrusted.example')
    bad_key, bad_host = certificate('wrong-host', 'DNS:localhost')
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(trusted, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = str(server.server_port)
    try:
        subprocess.run([sys.argv[1], '--tls', 'https://localhost:' + port + '/dns-query', port,
                        trusted, wrong], check=True)
        assert Handler.dns_count >= 2 and Handler.health_count >= 2
    finally:
        server.shutdown()
        thread.join()
        server.server_close()

    # 同一受信任证书覆盖 DoH 名称但不覆盖账号域名：请求必须在 TLS 阶段失败。
    bad_server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    bad_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    bad_context.load_cert_chain(bad_host, bad_key)
    bad_server.socket = bad_context.wrap_socket(bad_server.socket, server_side=True)
    bad_thread = threading.Thread(target=bad_server.serve_forever, daemon=True)
    bad_thread.start()
    bad_port = str(bad_server.server_port)
    try:
        subprocess.run([sys.argv[1], '--tls-bad-host',
                        'https://localhost:' + bad_port + '/dns-query', bad_port, bad_host], check=True)
    finally:
        bad_server.shutdown()
        bad_thread.join()
        bad_server.server_close()
