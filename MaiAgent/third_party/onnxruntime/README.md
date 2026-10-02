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

The Windows x64 CPU runtime is extracted from the official
[`onnxruntime-win-x64-1.26.0.zip`](https://github.com/microsoft/onnxruntime/releases/download/v1.26.0/onnxruntime-win-x64-1.26.0.zip)
release (archive SHA-256
`6ebe99b5564bf4d029b6e93eac9ff423682b6212eade769e9ca3f685eaf500b4`).
Only `onnxruntime.dll` and its `onnxruntime_providers_shared.dll` companion are
vendored in `windows-x64/`; the debug PDBs and import libraries are not needed
because MaiAgent loads the C API dynamically. Their respective SHA-256 values
are `b2ba7ca16e0e4fe71ad5148744ab885a2f5809e52a0c3de4d9ba3853a03977f9`
and `679ca9a81354d15e150f885a3ce1b52e08e05d2f7e8d8cb5fab32be1f1f9eecb`.
The app bundle includes the MIT license and third-party notices beside the RVM
model license.
