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
desktop/qt-im/build/multi_ai_im_desktop
```

## Native IM SDK

The desktop app is an independent Qt client. It does not depend on the Electron app or any local bridge process.

At runtime it loads the bundled native desktop IM SDK by default:

- macOS: `vendor/tencent-im/macos/ImSDKForMac_Plus.framework`
- Windows: `vendor/tencent-im/windows/shared_lib/Win64/ImSDK.dll`
- Windows 32-bit fallback: `vendor/tencent-im/windows/shared_lib/Win32/ImSDK.dll`

You can override the library path when debugging:

```bash
export MULTI_AI_IM_SDK_LIBRARY=/path/to/native/im/sdk/library
desktop/qt-im/build/multi_ai_im_desktop
```

The login screen requires:

- IM account ID

SDK AppID and SecretKey are bundled with the same defaults as the iOS app. The app generates UserSig locally with the TLS v2 algorithm.

For local development, these fields can be prefilled with:

```bash
export MULTI_AI_IM_USER_ID=<user-id>
```

Set `MULTI_AI_IM_USE_FAKE_CLIENT=ON` only when developing the UI without a native SDK.

The bundled SDK files come from the domestic Tencent download host `im.sdk.qcloud.com`; see `vendor/tencent-im/README.md`.
