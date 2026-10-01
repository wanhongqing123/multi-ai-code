# Vendored media codec sources

These sources are built from the repository into local build directories. No
system package manager or build-time download is required.

| Directory | Upstream source | Pinned revision / digest | License |
|---|---|---|---|
| `dav1d/` | [VideoLAN dav1d 1.5.4](https://download.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz) | SHA-256 `686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5` | BSD 2-Clause (`dav1d/COPYING`) |
| `x264/` | [VideoLAN x264 stable](https://code.videolan.org/videolan/x264) | Git `b35605ace3ddf7c1a5d67a2eb553f034aef41d55` | GPLv2 or later (`x264/COPYING`) |
| `lame/` | [LAME 3.101](https://sourceforge.net/projects/lame/files/lame/3.101/lame-3.101.tar.gz/download) | SHA-256 `7578af6eebd578b2bd64e468fac4ae1f03670a7e028166e67f855674b9b6aeac` | Library GPLv2 (`lame/COPYING`) |

The GPL-enabled FFmpeg build is a separate distribution decision from the
previous LGPL-only configuration. Release packaging must include the applicable
notices, source and build materials for the combined binaries.
