#!/usr/bin/env python3
"""Build and play generated HEVC/HLG MOV controls through the real FFplay bridge.

This test stores no user video. Build maichat_ffplay_hosted before invoking it:
  cmake --build MaiChat/desktop/build-release --target maichat_ffplay_hosted -j4
  python3 MaiChat/Media/tests/run_rotated_hdr_playback_test.py \
      --library MaiChat/desktop/build-release/libmaichat_ffplay_hosted.dylib
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


REPO = Path(__file__).resolve().parents[3]
TEST_SOURCE = Path(__file__).with_name("MaiFfplayRotatedHdrPlaybackTests.cpp")
BRIDGE_SOURCE = REPO / "MaiChat/ios/MaiChat/MaiChat/Media/MaiFfplayIosBridge.cpp"
HOST_SOURCES = (
    REPO / "MaiChat/Media/ffplay.c",
    REPO / "MaiChat/Media/FfplayCompat/MaiFfplayEntry.c",
    REPO / "MaiChat/Media/FfplayCompat/MaiFfplayRendererBackend.cpp",
)


def run(args, *, capture=False):
    return subprocess.run(args, check=True, text=True,
                          stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                          stderr=subprocess.PIPE if capture else subprocess.DEVNULL)


def probe(path):
    result = run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                  "-show_streams", "-of", "json", str(path)], capture=True)
    streams = json.loads(result.stdout)["streams"]
    if len(streams) != 1:
        raise RuntimeError(f"expected one video stream in {path.name}")
    return streams[0]


def verify(path, *, hdr, rotated):
    stream = probe(path)
    if hdr:
        expected = {
            "codec_name": "hevc", "profile": "Main 10",
            "pix_fmt": "yuv420p10le", "color_space": "bt2020nc",
            "color_transfer": "arib-std-b67", "color_primaries": "bt2020",
        }
    else:
        expected = {"codec_name": "h264", "pix_fmt": "yuv420p"}
    if any(stream.get(key) != value for key, value in expected.items()):
        raise RuntimeError(f"fixture metadata differs: {path.name}: {stream}")
    rotations = [side.get("rotation") for side in stream.get("side_data_list", [])
                 if side.get("side_data_type") == "Display Matrix"]
    if rotations != ([-180] if rotated else []):
        raise RuntimeError(f"unexpected display matrix in {path.name}: {rotations}")


def run_case(exe, path, mode, label, expected_ambient=None):
    command = [str(exe), str(path), mode]
    if expected_ambient:
        command.append(expected_ambient)
    result = subprocess.run(command, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=15)
    print(f"{label} {result.stdout.strip()}", flush=True)
    if result.returncode:
        relevant = [line.strip() for line in result.stderr.splitlines()
                    if "Impossible to convert" in line or
                       "movie produced" in line or
                       "filter link failed" in line or
                       "lost native" in line or
                       "ambient metadata" in line]
        raise RuntimeError(f"{label} failed ({result.returncode}): " +
                           "; ".join(relevant[-5:]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path,
                        help="freshly built macOS libmaichat_ffplay_hosted.dylib")
    parser.add_argument("--original", type=Path,
                        help="optional private source MOV; read in place, never copied")
    args = parser.parse_args()
    library = args.library.resolve()
    if not library.is_file():
        parser.error(f"hosted FFplay library missing: {library}")
    newest_source = max(path.stat().st_mtime for path in HOST_SOURCES)
    if library.stat().st_mtime < newest_source:
        parser.error("hosted FFplay library predates its source; rebuild first")

    with tempfile.TemporaryDirectory(prefix="maichat-rotated-hlg-gate-") as directory:
        temp = Path(directory)
        base = temp / "straight-hlg.mov"
        rotated = temp / "rotated-hlg.mov"
        sdr = temp / "ordinary-sdr.mov"
        exe = temp / "MaiFfplayRotatedHdrPlaybackTests"
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
             "-i", "testsrc2=size=192x108:rate=24:duration=2", "-vf",
             "format=yuv420p10le,setparams=color_primaries=bt2020:"
             "color_trc=arib-std-b67:colorspace=bt2020nc", "-c:v", "libx265",
             "-preset", "ultrafast", "-x265-params",
             "log-level=error:pools=1:frame-threads=1:colorprim=9:transfer=18:colormatrix=9",
             "-tag:v", "hvc1", "-an", "-y", str(base)])
        run(["ffmpeg", "-hide_banner", "-loglevel", "error",
             "-display_rotation:v:0", "-180", "-i", str(base),
             "-c", "copy", "-y", str(rotated)])
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
             "-i", "testsrc2=size=192x108:rate=24:duration=2", "-c:v", "libx264",
             "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-an", "-y", str(sdr)])
        verify(base, hdr=True, rotated=False)
        verify(rotated, hdr=True, rotated=True)
        verify(sdr, hdr=False, rotated=False)
        original = args.original.resolve() if args.original else None
        if original:
            if not original.is_file():
                parser.error("original MOV does not exist")
            verify(original, hdr=True, rotated=True)
        run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
             str(TEST_SOURCE), str(BRIDGE_SOURCE), "-L", str(library.parent),
             "-lmaichat_ffplay_hosted", f"-Wl,-rpath,{library.parent}",
             "-o", str(exe)], capture=True)
        for path, mode in ((base, "straight-hdr"), (rotated, "rotated-hdr"),
                           (sdr, "sdr")):
            run_case(exe, path, mode, mode)
        if original:
            run_case(exe, original, "rotated-hdr", "original",
                     expected_ambient="002fe9a03d134042")
    print("rotated HDR playback gate: PASS")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"rotated HDR playback gate: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
