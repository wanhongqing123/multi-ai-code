# ONNX Runtime C API

`include/onnxruntime_c_api.h` comes from ONNX Runtime `v1.26.0`:

<https://github.com/microsoft/onnxruntime/blob/v1.26.0/include/onnxruntime/core/session/onnxruntime_c_api.h>

It is licensed under MIT (`LICENSE`). The header is used by MaiAgent's shared
video-matting implementation. The application must bundle a matching ONNX Runtime
library for each target platform; a host-installed Python package is never used
by a shipped app.

The macOS arm64 dylib is from the official
[`onnxruntime-osx-arm64-1.26.0.tgz`](https://github.com/microsoft/onnxruntime/releases/download/v1.26.0/onnxruntime-osx-arm64-1.26.0.tgz)
release. Its SHA-256 is
`30afadcfc3c704f7671f8430d6252956651c1972373901d2be629da2e6a4d8ee`.
The accompanying `ThirdPartyNotices.txt` is included here.
