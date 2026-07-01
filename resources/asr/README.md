# ASR Runtime Assets

`npm run prepare-asr` populates this directory before packaging.

Generated files are intentionally ignored by git:

- `darwin-arm64/bin/whisper-cli`
- `darwin-arm64/bin/libggml-*.so`
- `darwin-arm64/lib/*.dylib`
- `win32-x64/bin/whisper-cli.exe` and its DLLs
- `win32-x64/bin/ffmpeg.exe`
- `models/ggml-base.bin`

The generated assets are included in packaged apps through `build.extraResources`.
