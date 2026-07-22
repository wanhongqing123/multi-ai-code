# Desktop IM

Qt5 Widgets based IM client for Windows and macOS.

## Build

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build -DCMAKE_PREFIX_PATH=/path/to/qt5
cmake --build desktop/qt-im/build
ctest --test-dir desktop/qt-im/build --output-on-failure
```

On local Homebrew macOS setups:

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@5
cmake --build desktop/qt-im/build
```

## Run

```bash
desktop/qt-im/build/maichat_desktop
```

## Native IM SDK

The desktop app is an independent Qt client. It does not depend on the Electron app or any local bridge process.

At runtime it loads the bundled native desktop IM SDK by default:

- macOS: `vendor/tencent-im/macos/ImSDKForMac_Plus.framework`
- Windows: `vendor/tencent-im/windows/shared_lib/Win64/ImSDK.dll`
- Windows 32-bit fallback: `vendor/tencent-im/windows/shared_lib/Win32/ImSDK.dll`

You can override the library path when debugging:

```bash
export MAICHAT_SDK_LIBRARY=/path/to/native/im/sdk/library
desktop/qt-im/build/maichat_desktop
```

The login screen requires:

- IM account ID

SDK AppID and SecretKey are bundled with the same defaults as the iOS app. The app generates UserSig locally with the TLS v2 algorithm.

For local development, these fields can be prefilled with:

```bash
export MAICHAT_USER_ID=<user-id>
```

Set `MAICHAT_USE_FAKE_CLIENT=ON` only when developing the UI without a native SDK.

The bundled SDK files come from the domestic Tencent download host `im.sdk.qcloud.com`; see `vendor/tencent-im/README.md`.

## Windows 免安装绿色包

无需在目标机器安装 Qt 或 VC++ 运行库（要求 Windows 10 及以上 64 位）。先完成 Release 构建，然后：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1
```

产出 `dist\MaiChat-win64\`（解压即用目录）和 `dist\MaiChat-win64-<日期>-<git短哈希>.zip`。脚本会：

- `windeployqt` 旁挂 Qt 运行时（platforms/styles/imageformats 等）；
- 从本机 VS Redist 显式拷贝最高版本的 VC++ CRT（`msvcp140.dll`、`vcruntime140*.dll` 等，app-local 部署）；
- 按 `vendor/tencent-im/windows/shared_lib/Win64/` 相对路径附带 ImSDK.dll（exe 按此路径探测）；
- 生成 `使用说明.txt` 并压缩打包。

验证方式：用只含 `C:\Windows\System32` 的裸 PATH 启动包内 exe，进程存活即依赖完整（缺 DLL 会立即以 0xc0000135 退出）。

产出的 zip 不提交进仓库（`dist/` 已被 gitignore），通过 GitHub Releases 发布下载：
<https://github.com/wanhongqing123/multi-ai-code/releases>（tag 形如 `qt-im-<日期>`）。

## macOS 免安装包

先完成 Release 构建：

```bash
cmake -S desktop/qt-im -B desktop/qt-im/build \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@5 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build desktop/qt-im/build --target maichat_desktop --config Release
```

然后打包：

```bash
desktop/qt-im/scripts/package-macos.sh
```

产出 `dist/MaiChat-macos-arm64/` 和
`dist/MaiChat-macos-arm64-<日期>-<git短哈希>.dmg`。DMG 内包含应用和
`Applications` 快捷入口，脚本会：

- 使用 `macdeployqt` 旁挂 Qt framework；
- 把原生 IM SDK framework 放进应用内置 vendor 目录；
- 生成标准 Finder 安装窗口，提示把应用拖到 `Applications`；
- 校验 `CFBundleVersion` 与 `CFBundleShortVersionString`，确保 Launchpad 可索引；
- 对 `.app` 做 ad-hoc codesign，避免嵌入 framework 因未签名无法加载；
- 生成 `使用说明.txt` 并压缩打包。

产出的 zip 不提交进仓库，通过 GitHub Releases 发布下载：

```bash
npm run release:mac
```

该命令需要在仓库根目录执行，并依赖已登录的 GitHub CLI。
