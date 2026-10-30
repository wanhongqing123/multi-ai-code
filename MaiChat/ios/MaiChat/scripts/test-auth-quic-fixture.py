#!/usr/bin/env python3
"""本机账号 QUIC 测试服务；只接收测试正文，不使用真实账号或凭据。

运行需要测试环境安装 aioquic，并提供域名为 ichat.life 的临时证书与私钥。
同一端口提供 HTTP/3 和 HTTP/1.1，另一端口仅提供 HTTP/1.1，用于验证回退。
"""
import argparse
import asyncio
import json
import ssl
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

from aioquic.asyncio import serve
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration


def record(protocol, method, body):
    # 测试服务只运行在回环地址，日志仅包含调用方指定的合成测试正文。
    print(json.dumps({"protocol": protocol, "method": method, "body": body.decode()}), flush=True)


class TcpHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        record("h1", "POST", body)
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *_args):
        pass


class QuicHandler(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.http = H3Connection(self._quic)
        self.requests = {}

    def quic_event_received(self, event):
        for item in self.http.handle_event(event):
            if isinstance(item, HeadersReceived):
                self.requests[item.stream_id] = {
                    "method": dict(item.headers).get(b":method", b"").decode(), "body": b""
                }
                if item.stream_ended:
                    self.respond(item.stream_id)
            elif isinstance(item, DataReceived):
                self.requests[item.stream_id]["body"] += item.data
                if item.stream_ended:
                    self.respond(item.stream_id)

    def respond(self, stream):
        request = self.requests.pop(stream)
        record("h3", request["method"], request["body"])
        if request["body"] == b"ambiguous-fixture-body":
            # 模拟服务端收到写请求后断开：客户端不能把它重发到 HTTP/1.1。
            self._quic.close(error_code=1, reason_phrase="intentional disconnect")
        else:
            self.http.send_headers(stream, [(b":status", b"200"), (b"content-length", b"2")])
            self.http.send_data(stream, b"ok", end_stream=True)
        self.transmit()


def serve_tcp(port, certificate, key):
    server = HTTPServer(("127.0.0.1", port), TcpHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    server.serve_forever()


async def main(args):
    for port in (args.quic_port, args.fallback_port):
        threading.Thread(target=serve_tcp, args=(port, args.certificate, args.key), daemon=True).start()
    config = QuicConfiguration(is_client=False, alpn_protocols=H3_ALPN)
    config.load_cert_chain(args.certificate, args.key)
    await serve("127.0.0.1", args.quic_port, configuration=config, create_protocol=QuicHandler)
    print("ready", flush=True)
    await asyncio.Event().wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--certificate", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--quic-port", type=int, required=True)
    parser.add_argument("--fallback-port", type=int, required=True)
    asyncio.run(main(parser.parse_args()))
