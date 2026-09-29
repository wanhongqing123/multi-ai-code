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
`maiagent_graphics` does not compile or link it yet. The upstream Graphics
files depend on other libobs interfaces and SIMDe; those dependencies must be
resolved before replacing the current experimental adapter with the upstream
implementation. No other OBS directories were copied.
