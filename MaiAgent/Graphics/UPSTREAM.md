# OBS Graphics source snapshot

Source: `/Users/hongqingwan/OpenSource/obs-studio`, commit
`50530ce9046599e698c5d2068e4f053fae2318f6` of
[obsproject/obs-studio](https://github.com/obsproject/obs-studio/tree/50530ce9046599e698c5d2068e4f053fae2318f6).

Source directory mapping into `MaiAgent/Graphics/`:

| OBS source | This directory |
| --- | --- |
| `libobs/graphics/` | root |
| `libobs/util/` | `util/` |
| `libobs/data/` | `data/` |
| `libobs-d3d11/` | `d3d11/` |
| `libobs-opengl/` | `opengl/` |
| `libobs-metal/` | `metal/` |
| `COPYING` | `COPYING` |

All 21 `libobs/data/*.effect` files were copied without modification.
Include paths affected by the flattened layout were adjusted in source files.
The standalone build guards `util/platform.c`'s OBS-application-only filename
formatter behind `MAI_GRAPHICS_STANDALONE`. The Metal backend also has iOS
UIKit and static-symbol adaptations. Original copyright notices were preserved.

The source files retain their original copyright notices. `COPYING` is the
upstream GPLv2 license text. SIMDe is provided separately under
`MaiAgent/third_party/simde`, matching the version used by OBS. The optional
Graphics build links the upstream Graphics C sources, including
`graphics-ffmpeg.c` on macOS against vendored FFmpeg 9.0.2. The OBS Metal
Swift backend is a separate loadable module on macOS Apple Silicon and a
statically linked backend on iOS. Standalone
application callbacks are isolated in `compat/`; device-reset signaling and
HDR video information remain to be integrated with the host application.
No other OBS directories were copied.
