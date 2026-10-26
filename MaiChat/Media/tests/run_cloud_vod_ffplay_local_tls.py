#!/usr/bin/env python3
"""Play a moov-at-end MOV through the shared HTTPS Range source and FFplay."""

import ssl
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Network/tests"))
from run_cloud_vod_local_tls import Fixture  # noqa: E402


def has_moov_after_mdat(path):
    atoms = []
    with path.open("rb") as movie:
        while True:
            header = movie.read(8)
            if len(header) != 8:
                break
            size, name = struct.unpack(">I4s", header)
            if size == 1:
                size = struct.unpack(">Q", movie.read(8))[0]
            if size < 8:
                break
            atoms.append(name)
            movie.seek(size - (16 if header[:4] == b"\x00\x00\x00\x01" else 8), 1)
    return b"mdat" in atoms and b"moov" in atoms and \
        atoms.index(b"moov") > atoms.index(b"mdat")


def main():
    if len(sys.argv) != 5:
        raise SystemExit("expected hosted test, graphics backend, effects, ffmpeg")
    with tempfile.TemporaryDirectory(prefix="maichat-vod-ffplay-") as directory:
        directory = Path(directory)
        movie = directory / "moov-at-end.mov"
        subprocess.run([sys.argv[4], "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i",
                        "testsrc2=size=1280x720:rate=30:duration=14",
                        "-f", "lavfi", "-i",
                        "sine=frequency=660:sample_rate=48000:duration=14",
                        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "7",
                        "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k",
                        "-shortest", str(movie)], check=True, timeout=30)
        if movie.stat().st_size < 20 * 1024 * 1024 or not has_moov_after_mdat(movie):
            raise RuntimeError("MOV fixture must exceed 20 MiB with moov at the end")
        cert = directory / "cert.pem"
        key = directory / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                        "-nodes", "-days", "1", "-keyout", str(key),
                        "-out", str(cert), "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost"],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        server = Fixture()
        server.payload = movie.read_bytes()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(cert), str(key))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            subprocess.run([sys.argv[1], sys.argv[2], sys.argv[3],
                            f"https://localhost:{server.server_port}",
                            str(cert), str(movie.stat().st_size)],
                           check=True, timeout=55)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    main()
