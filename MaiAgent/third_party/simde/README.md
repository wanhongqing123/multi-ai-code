# SIMDe 0.8.2

This header-only copy comes from the OBS local dependency bundle
`obs-deps-2025-08-23-universal/include/simde/`, the exact headers used by the
local OBS build at `50530ce9046599e698c5d2068e4f053fae2318f6`.
`simde-common.h` reports version 0.8.2. The header tree was compared with the
OBS dependency bundle after copying. Upstream:
[SIMD Everywhere v0.8.2](https://github.com/simd-everywhere/simde/releases/tag/v0.8.2).

OBS's `libobs/util/sse-intrin.h` includes `simde/x86/sse2.h` with
`SIMDE_ENABLE_NATIVE_ALIASES` on platforms without native x86 SSE2. This lets
its `_mm_*`-based graphics math compile on ARM, where SIMDe can use NEON.

Keep the upstream `licenses/COPYING` and `licenses/CC0-1.0.txt` with these
headers. `include/` is the only required include root; no binary library is
built or linked.
