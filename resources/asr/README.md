# ASR Runtime Assets

`npm run prepare-asr` populates this directory before packaging.

The shared Whisper model is tracked by Git LFS:

- `models/ggml-base.bin`

Generated runtime binaries are intentionally ignored by git:

- `darwin-arm64/bin/whisper-cli`
- `darwin-arm64/bin/libggml-*.so`
- `darwin-arm64/lib/*.dylib`
- `win32-x64/bin/whisper-cli.exe` and its DLLs
- `win32-x64/bin/ffmpeg.exe`

The generated assets are included in packaged apps through `build.extraResources`.
