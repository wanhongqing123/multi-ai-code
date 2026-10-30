#!/usr/bin/env python3
"""隔离 TLS 服务器验证备用地址不会改变域名身份，也不会重放写帧。"""
import base64
import hashlib
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def run_fixture(binary, directory, hostname, reject, first_route_fails=False):
    cert, key = directory / f'{hostname}.pem', directory / f'{hostname}.key'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(key), '-out', str(cert), '-days', '1',
                    '-subj', f'/CN={hostname}', '-addext', f'subjectAltName=DNS:{hostname}'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(5)
    listener.settimeout(15)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    snis, requests, frames = [], [], []
    ctx.set_servername_callback(lambda _sock, name, _context: snis.append(name))

    def serve():
        try:
            for _ in range(2):
                client, _ = listener.accept()
                try:
                    with ctx.wrap_socket(client, server_side=True) as conn:
                        conn.settimeout(3)
                        request = b''
                        while b'\r\n\r\n' not in request:
                            request += conn.recv(4096)
                        requests.append(request)
                        headers = dict(line.split(b': ', 1) for line in request.split(b'\r\n')[1:] if b': ' in line)
                        accept = base64.b64encode(hashlib.sha1(headers[b'Sec-WebSocket-Key'] +
                            b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
                        conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                                     b'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
                        try:
                            frames.append(conn.recv(4096))
                        except (TimeoutError, socket.timeout):
                            frames.append(b'')
                except (ssl.SSLError, OSError):
                    pass
        except (TimeoutError, OSError):
            pass

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    env = {**os.environ, 'MAICHAT_TEST_WSS_ADDRESS': '127.0.0.2' if first_route_fails else '127.0.0.1',
           'MAICHAT_TEST_WSS_SECOND_ADDRESS': '127.0.0.1'}
    result = subprocess.run([str(binary), str(listener.getsockname()[1]), str(cert),
                             'reject' if reject else 'accept'], env=env, timeout=20)
    listener.close()
    worker.join(4)
    assert result.returncode == 0, result.returncode
    assert snis and all(name == 'ichat.life' for name in snis), snis
    if reject:
        assert not requests, 'wrong-host certificate received an HTTP request'
    else:
        assert len(requests) == 2, len(requests)
        assert all(b'Host: ichat.life:' in r and b'Bearer local-test-token' in r for r in requests)
        assert len(frames) == 2 and frames[0] and not frames[1], frames


def main():
    with tempfile.TemporaryDirectory(prefix='maichat-wss-route-') as tmp:
        directory = Path(tmp)
        binary = directory / 'test'
        include = ROOT / 'MaiAgent' / 'third_party'
        subprocess.run(['clang++', '-std=c++17', '-DASIO_STANDALONE', '-D_WEBSOCKETPP_CPP11_STRICT_',
                        '-DMAICHAT_SESSION_SYNC_TEST', '-I' + str(ROOT / 'MaiChat/SessionSync'),
                        '-I' + str(ROOT / 'MaiAgent/include'), '-I' + str(include / 'websocketpp'),
                        '-I' + str(include / 'asio/asio/include'), '-I/opt/homebrew/opt/openssl@3/include',
                        str(ROOT / 'MaiChat/SessionSync/MaiSessionSyncSocket.cpp'),
                        str(ROOT / 'MaiChat/SessionSync/tests/RoutedSocketTest.cpp'),
                        '-L/opt/homebrew/opt/openssl@3/lib', '-lcurl', '-lssl', '-lcrypto', '-o', str(binary)],
                       check=True)
        run_fixture(binary, directory, 'ichat.life', False)
        # 首个 DoH 地址的 TCP 连接失败时必须尝试第二个地址，不能一直重复首路。
        run_fixture(binary, directory, 'ichat.life', False, first_route_fails=True)
        run_fixture(binary, directory, 'wrong.example', True)
    print('WSS route, SNI, Host, certificate rejection and no replay: PASS')


if __name__ == '__main__':
    sys.exit(main())
