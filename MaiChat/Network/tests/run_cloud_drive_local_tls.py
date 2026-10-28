#!/usr/bin/env python3
"""Exercise the shared resumable cloud client against disposable TLS auth/OSS."""

import base64
import hashlib
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
import uuid
from urllib.parse import parse_qs, urlsplit


VIDEO_ID = "123e4567-e89b-42d3-a456-426614174000"
PART_SIZE = 8 * 1024 * 1024


class Fixture(http.server.ThreadingHTTPServer):
    def __init__(self, address):
        super().__init__(address, Handler)
        self.lock = threading.Lock()
        self.part_bytes = {}
        self.signs = {}
        self.sessions = 0
        self.complete_calls = 0
        self.slow_complete = True
        self.oss_authorization = 0
        self.range_requests = 0
        self.full_downloads = 0
        self.redirect_followed = 0
        self.status = "none"
        self.name = ""
        self.content_type = ""
        self.size = 0
        self.room_key = ""
        self.folders = {}
        self.folder_id = None
        self.folder_response_case = ""

    def media_type(self):
        if self.content_type.startswith("image/"):
            return "image"
        if self.content_type.startswith("audio/"):
            return "audio"
        if self.content_type.startswith("video/"):
            return "video"
        return "file"

    def handle_error(self, *_args):
        # Cancel tests intentionally close sockets during an OSS PUT.
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def reply(self, status, body):
        payload = json.dumps(body, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    def account(self):
        return {"Bearer author-token": "author", "Bearer viewer-token": "viewer"}.get(
            self.headers.get("Authorization"))

    def json_body(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > 4096:
            return None
        try:
            return json.loads(self.rfile.read(length) or b"{}")
        except (json.JSONDecodeError, UnicodeDecodeError):
            return None

    def path_only(self):
        return urlsplit(self.path).path.replace(
            "/auth/v1/session-attachments", "/auth/v1/cloud-drive/videos")

    def origin(self):
        return f"https://localhost:{self.server.server_port}"

    def folder_request(self, method, body=None):
        path = self.path_only()
        root = "/auth/v1/cloud-drive/folders"
        if path != root and not path.startswith(root + "/"):
            return False
        account = self.account()
        if not account:
            self.reply(401, {"error": "auth_required"})
            return True
        folder_id = path[len(root) + 1:] if path != root else None
        body = body or {}
        with self.server.lock:
            folders = self.server.folders
            current = folders.get(folder_id)
            def public(folder):
                value = {key: value for key, value in folder.items() if key != "owner"}
                case = self.server.folder_response_case
                if case == "missing_created":
                    value.pop("created_at", None)
                elif case == "string_created":
                    value["created_at"] = "1793181600000"
                elif case == "negative_updated":
                    value["updated_at"] = -1
                return value
            def parent_valid(parent):
                return parent is None or (parent in folders and folders[parent]["owner"] == account)
            if method == "GET" and folder_id is None:
                self.reply(200, {"folders": [public(folder) for folder in folders.values()
                                            if folder["owner"] == account]})
            elif method == "POST" and folder_id is None:
                name, parent = body.get("name"), body.get("parent_id") or None
                if not isinstance(name, str) or not name or "/" in name or "\\" in name:
                    self.reply(400, {"error": "invalid_name"})
                elif not parent_valid(parent):
                    self.reply(404, {"error": "parent_not_found"})
                elif any(f["owner"] == account and f["name"] == name and
                         f["parent_id"] == parent for f in folders.values()):
                    self.reply(409, {"error": "duplicate_name"})
                else:
                    folder_id = str(uuid.uuid4())
                    folder = {"id": folder_id, "name": name, "parent_id": parent,
                              "created_at": 1793181600000, "updated_at": 1793181600000,
                              "owner": account}
                    folders[folder_id] = folder
                    self.reply(201, public(folder))
            elif not current or current["owner"] != account:
                self.reply(404, {"error": "not_found"})
            elif method == "PATCH":
                parent = (body.get("parent_id") or None) if "parent_id" in body else current["parent_id"]
                name = body.get("name", current["name"])
                ancestor = parent
                cycle = False
                while ancestor in folders:
                    if ancestor == folder_id:
                        cycle = True
                        break
                    ancestor = folders[ancestor]["parent_id"]
                if not parent_valid(parent):
                    self.reply(404, {"error": "parent_not_found"})
                elif cycle or any(f["owner"] == account and f["id"] != folder_id and
                                  f["parent_id"] == parent and f["name"] == name
                                  for f in folders.values()):
                    self.reply(409, {"error": "invalid_move_or_duplicate"})
                else:
                    current.update(name=name, parent_id=parent)
                    self.reply(200, public(current))
            elif method == "DELETE":
                nonempty = any(f["parent_id"] == folder_id for f in folders.values()) or (
                    account == "author" and self.server.folder_id == folder_id and
                    self.server.status in ("ready", "uploading", "completing"))
                if nonempty:
                    self.reply(409, {"error": "Folder is not empty"})
                else:
                    del folders[folder_id]
                    self.reply(200, {"folder_id": folder_id, "status": "deleted"})
            else:
                self.reply(404, {"error": "not_found"})
        return True

    def do_GET(self):
        path = self.path_only()
        if path == "/test/folder-response":
            case = parse_qs(urlsplit(self.path).query).get("case", [""])[0]
            with self.server.lock:
                self.server.folder_response_case = case
            self.reply(200, {"case": case})
            return
        if path == "/test/state":
            with self.server.lock:
                self.reply(200, {"sessions": self.server.sessions,
                    "parts": sorted(self.server.part_bytes),
                    "oss_authorization": self.server.oss_authorization,
                    "redirect_followed": self.server.redirect_followed,
                    "status": self.server.status})
            return
        if path == "/test/complete-calls":
            with self.server.lock:
                calls = self.server.complete_calls
            self.reply(200, {"complete_calls": calls})
            return
        if path == "/oss/play":
            with self.server.lock:
                content = b"".join(self.server.part_bytes.values())
            range_header = self.headers.get("Range")
            if range_header:
                with self.server.lock:
                    self.server.range_requests += 1
                match = re.fullmatch(r"bytes=(\d+)-(\d+)", range_header)
                if not match:
                    self.send_error(416)
                    return
                first, last = int(match.group(1)), int(match.group(2))
                if first >= len(content) or first > last:
                    self.send_error(416)
                    return
                if last >= len(content):
                    # Match production OSS: an overlong final Range can be
                    # ignored and return 200. The VOD client must retry with
                    # the exact EOF instead of accepting a whole-file body.
                    self.send_response(200)
                else:
                    self.send_response(206)
                    self.send_header("Content-Range", f"bytes {first}-{last}/{len(content)}")
                    content = content[first:last + 1]
            else:
                with self.server.lock:
                    self.server.full_downloads += 1
                self.send_response(200)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", '"fixture-play-object"')
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Content-Type", self.server.content_type)
            self.end_headers()
            self.wfile.write(content)
            return
        account = self.account()
        if not account:
            self.reply(401, {"error": "auth_required"})
            return
        if self.folder_request("GET"):
            return
        if path == "/auth/v1/cloud-drive/videos":
            with self.server.lock:
                videos = [{"id": VIDEO_ID, "name": self.server.name,
                    "size_bytes": self.server.size,
                    "content_type": self.server.content_type,
                    "media_type": self.server.media_type(),
                    "folder_id": self.server.folder_id,
                    "created_at": "2026-10-24T00:00:00Z"}]
                if account != "author" or self.server.status != "ready":
                    videos = []
                used = self.server.size if account == "author" and self.server.status == "ready" else 0
                query = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
                if "folder_id" in query and (query["folder_id"][0] or None) != self.server.folder_id:
                    videos = []
            self.reply(200, {"videos": videos, "next_cursor": None,
                "quota": {"used_bytes": used,
                          "reserved_bytes": 0, "limit_bytes": 10_000_000_000}})
            return
        if path == f"/auth/v1/cloud-drive/videos/uploads/{VIDEO_ID}/parts":
            if account != "author":
                self.reply(404, {"error": "not_found"})
                return
            with self.server.lock:
                parts = [{"part_number": number, "size_bytes": len(data),
                    "etag": hashlib.md5(data).hexdigest()}
                    for number, data in sorted(self.server.part_bytes.items())]
                status = self.server.status
                size = self.server.size
            self.reply(200, {"video_id": VIDEO_ID, "status": status,
                "part_size": PART_SIZE, "parts_total": (size + PART_SIZE - 1) // PART_SIZE,
                "parts": parts if status == "uploading" else [],
                "upload_expires_at": "2026-10-31T00:00:00Z"})
            return
        if path == f"/auth/v1/cloud-drive/videos/{VIDEO_ID}/play-link":
            with self.server.lock:
                ready = self.server.status == "ready"
                size = self.server.size
                content_type = self.server.content_type
            if account != "author" or not ready:
                self.reply(404, {"error": "not_found"})
                return
            self.reply(200, {"read_url": self.origin() + "/oss/play?signature=fixture",
                "read_expires_in": 60, "size_bytes": size,
                "content_type": content_type,
                "media_type": self.server.media_type(),
                "name": self.server.name, "room_key": self.server.room_key})
            return
        if path == f"/auth/v1/cloud-drive/videos/{VIDEO_ID}/thumbnail-link":
            with self.server.lock:
                ready = self.server.status == "ready"
            if account != "author" or not ready:
                self.reply(404, {"error": "not_found"})
                return
            self.reply(200, {"thumbnail_url": self.origin() +
                "/oss/thumbnail?signature=fixture&x-oss-process=image/resize,m_lfit,w_320,h_320",
                "thumbnail_expires_in": 60, "content_type": "image/jpeg"})
            return
        self.reply(404, {"error": "not_found"})

    def do_POST(self):
        path = self.path_only()
        session_mode = urlsplit(self.path).path.startswith(
            "/auth/v1/session-attachments")
        account = self.account()
        if not account:
            self.reply(401, {"error": "auth_required"})
            return
        body = self.json_body()
        if not isinstance(body, dict):
            self.reply(400, {"error": "invalid_json"})
            return
        if self.folder_request("POST", body):
            return
        if session_mode and path == "/auth/v1/cloud-drive/videos/cleanup-room":
            room = ":".join(str(body.get(key, "")) for key in (
                "owner_slot", "session_id"))
            if account != "author" or room != self.server.room_key:
                self.reply(404, {"error": "not_found"})
                return
            self.server.status = "deleted"
            self.reply(202, {"room_key": room, "deleted": 1})
            return
        if path == "/auth/v1/cloud-drive/videos/uploads":
            content_type = body.get("content_type")
            if (account != "author" or content_type not in (
                    "video/mp4", "video/quicktime", "image/jpeg", "image/png",
                    "image/webp", "image/heic") and not session_mode or
                    session_mode and content_type not in (
                        "video/mp4", "video/quicktime", "image/jpeg", "image/png",
                        "image/webp", "image/heic", "audio/mp4",
                        "application/octet-stream") or
                    not isinstance(body.get("size_bytes"), int) or
                    not 0 < body["size_bytes"] <=
                        (20_000_000 if content_type.startswith("image/") else 2_000_000_000)):
                self.reply(400, {"error": "invalid_media"})
                return
            with self.server.lock:
                self.server.sessions += 1
                self.server.status = "uploading"
                self.server.size = body["size_bytes"]
                self.server.name = body["name"]
                self.server.content_type = body["content_type"]
                self.server.folder_id = None
                self.server.room_key = ":".join(str(body.get(key, "")) for key in (
                    "owner_slot", "session_id")) if session_mode else ""
                self.server.part_bytes.clear()
            self.reply(201, {"video_id": VIDEO_ID, "upload_id": "oss-upload-1",
                "media_type": self.server.media_type(),
                "room_key": self.server.room_key,
                "part_size": PART_SIZE,
                "parts_total": (body["size_bytes"] + PART_SIZE - 1) // PART_SIZE,
                "upload_expires_at": "2026-10-31T00:00:00Z"})
            return
        signed = re.fullmatch(
            rf"/auth/v1/cloud-drive/videos/uploads/{VIDEO_ID}/parts/(\d+)/sign", path)
        if signed:
            if account != "author":
                self.reply(404, {"error": "not_found"})
                return
            number = int(signed[1])
            expected = min(PART_SIZE, self.server.size - (number - 1) * PART_SIZE)
            md5 = body.get("content_md5_base64")
            if not 1 <= number <= (self.server.size + PART_SIZE - 1) // PART_SIZE or \
                    not isinstance(md5, str):
                self.reply(400, {"error": "invalid_part"})
                return
            self.server.signs[number] = md5
            self.reply(200, {"part_number": number, "part_size_bytes": expected,
                "upload_url": self.origin() +
                    f"/oss/cloud-drive/author/{VIDEO_ID}.mov?partNumber={number}&uploadId=oss-upload-1&signature=fixture",
                "upload_headers": {"Content-Length": str(expected),
                                   "Content-MD5": md5}, "expires_in": 300})
            return
        if path == f"/auth/v1/cloud-drive/videos/uploads/{VIDEO_ID}/complete":
            if account != "author":
                self.reply(404, {"error": "not_found"})
                return
            with self.server.lock:
                expected = (self.server.size + PART_SIZE - 1) // PART_SIZE
                complete = all(number in self.server.part_bytes for number in range(1, expected + 1))
                if complete:
                    self.server.status = "ready"
                    self.server.complete_calls += 1
                    complete_calls = self.server.complete_calls
                name = self.server.name
                size = self.server.size
                content_type = self.server.content_type
            if not complete:
                self.reply(409, {"error": "incomplete"})
                return
            # The first call exercises the default beyond both prior 30-second
            # curl timers. The second checks that an explicit caller timeout wins.
            if self.server.slow_complete and complete_calls == 1:
                time.sleep(31.5)
            elif self.server.slow_complete and complete_calls == 2:
                time.sleep(1.5)
            elif self.server.slow_complete and complete_calls == 5:
                time.sleep(2.0)
            self.reply(200, {"video_id": VIDEO_ID, "status": "ready",
                "name": name, "size_bytes": size,
                "content_type": content_type,
                "media_type": self.server.media_type(),
                "room_key": self.server.room_key,
                "created_at": "2026-10-24T00:00:00Z"})
            return
        self.reply(404, {"error": "not_found"})

    def do_PUT(self):
        path = self.path_only()
        query = parse_qs(urlsplit(self.path).query)
        if path == "/redirect-followed":
            self.server.redirect_followed += 1
            self.reply(500, {"error": "redirect_followed"})
            return
        if not path.startswith("/oss/cloud-drive/") or \
                query.get("uploadId") != ["oss-upload-1"]:
            self.reply(404, {"error": "invalid_oss_path"})
            return
        if self.headers.get("Authorization") or self.headers.get("Cookie"):
            self.server.oss_authorization += 1
            self.reply(400, {"error": "credential_leak"})
            return
        number = int(query["partNumber"][0])
        length = int(self.headers.get("Content-Length", "0"))
        if number == 2:
            # Leave a large second part in flight long enough for cancellation.
            first = self.rfile.read(min(length, 64 * 1024))
            time.sleep(0.15)
            content = first + self.rfile.read(length - len(first))
        else:
            content = self.rfile.read(length)
        actual_md5 = base64.b64encode(hashlib.md5(content).digest()).decode()
        if length != len(content) or actual_md5 != self.headers.get("Content-MD5") or \
                actual_md5 != self.server.signs.get(number):
            self.reply(400, {"error": "bad_part"})
            return
        with self.server.lock:
            self.server.part_bytes[number] = content
        self.send_response(200)
        self.send_header("ETag", f'"{hashlib.md5(content).hexdigest()}"')
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_PATCH(self):
        body = self.json_body()
        if not isinstance(body, dict):
            self.reply(400, {"error": "invalid_json"})
            return
        if self.folder_request("PATCH", body):
            return
        account = self.account()
        if self.path_only() == f"/auth/v1/cloud-drive/videos/{VIDEO_ID}":
            with self.server.lock:
                folder_id = body.get("folder_id") or None
                target = self.server.folders.get(folder_id)
                if account != "author" or self.server.status != "ready" or (
                        folder_id and (not target or target["owner"] != account)):
                    self.reply(404, {"error": "not_found"})
                    return
                self.server.folder_id = folder_id
                response = {"video_id": VIDEO_ID, "folder_id": folder_id,
                    "status": "ready", "name": self.server.name, "size_bytes": self.server.size,
                    "content_type": self.server.content_type, "media_type": self.server.media_type(),
                    "created_at": 1793181600000}
                case = self.server.folder_response_case
                if case == "missing_status":
                    response.pop("status")
                elif case == "wrong_status":
                    response["status"] = "uploading"
                elif case == "missing_name":
                    response.pop("name")
                elif case == "negative_size":
                    response["size_bytes"] = -1
                elif case == "wrong_content_type":
                    response["content_type"] = "application/octet-stream"
                elif case == "wrong_media_type":
                    response["media_type"] = "image"
                elif case == "missing_created":
                    response.pop("created_at")
                self.reply(200, response)
            return
        self.reply(404, {"error": "not_found"})

    def do_DELETE(self):
        account = self.account()
        path = self.path_only()
        if self.folder_request("DELETE"):
            return
        if account != "author":
            self.reply(404, {"error": "not_found"})
            return
        if path == f"/auth/v1/cloud-drive/videos/{VIDEO_ID}":
            with self.server.lock:
                self.server.status = "deleted"
            self.reply(200, {"video_id": VIDEO_ID, "status": "deleted",
                             "cleanup_pending": False})
            return
        if path == f"/auth/v1/cloud-drive/videos/uploads/{VIDEO_ID}":
            with self.server.lock:
                self.server.status = "deleted"
            self.reply(200, {"video_id": VIDEO_ID, "status": "deleted",
                             "cleanup_pending": False})
            return
        self.reply(404, {"error": "not_found"})


def main():
    if len(sys.argv) != 2:
        raise SystemExit("expected cloud-drive C++ test executable")
    with tempfile.TemporaryDirectory(prefix="maichat-cloud-test-") as directory:
        cert = os.path.join(directory, "server.pem")
        key = os.path.join(directory, "server.key")
        unrelated_cert = os.path.join(directory, "unrelated.pem")
        unrelated_key = os.path.join(directory, "unrelated.key")
        for crt, private in ((cert, key), (unrelated_cert, unrelated_key)):
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                "-sha256", "-nodes", "-days", "1", "-subj", "/CN=localhost",
                "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                "-keyout", private, "-out", crt], stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, check=True)
        server = Fixture(("127.0.0.1", 0))
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            return subprocess.run([sys.argv[1],
                f"https://localhost:{server.server_port}", cert,
                unrelated_cert, directory], timeout=90, check=False).returncode
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
