# OBS Graphics source snapshot

Source: `/Users/hongqingwan/OpenSource/obs-studio`, commit
`50530ce9046599e698c5d2068e4f053fae2318f6` of
[obsproject/obs-studio](https://github.com/obsproject/obs-studio/tree/50530ce9046599e698c5d2068e4f053fae2318f6).

Copied without editing the upstream files:

- `libobs/graphics/`
- `libobs/util/`
- `libobs-d3d11/`
- `libobs-opengl/`
- `libobs-metal/`
- `COPYING`

The source files retain their original copyright notices. `COPYING` is the
upstream GPLv2 license text. This snapshot is currently reference source:
`maiagent_graphics` does not link it yet. SIMDe is provided separately under
`MaiAgent/third_party/simde`, matching the version used by OBS. A compile-only
target checks the upstream Graphics C files except `graphics-ffmpeg.c`; that
file additionally needs `obs-ffmpeg-compat.h` from outside the requested OBS
source directories. Runtime linking still requires other libobs symbols and
backend integration. No other OBS directories were copied.
