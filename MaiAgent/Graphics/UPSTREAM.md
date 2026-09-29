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
upstream GPLv2 license text. SIMDe is provided separately under
`MaiAgent/third_party/simde`, matching the version used by OBS. The optional
Graphics build compiles the upstream Graphics C sources, including
`graphics-ffmpeg.c` on macOS using a local compatibility header and FFmpeg
generated headers. OBS math and lexer code is linked into focused test
executables. The complete libobs runtime and platform backends are not linked
or exposed as a MaiAgent API yet. No other OBS directories were copied.
