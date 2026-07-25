# Tencent IM Desktop SDK

This directory contains the native desktop IM SDK used by the standalone Qt IM app.

Sources:

- macOS: `https://im.sdk.qcloud.com/download/plus/9.0.7652/ImSDKForMac_Plus_9.0.7652.framework.zip`
- Windows: `https://im.sdk.qcloud.com/download/plus/9.0.7652/cross_platform/ImSDK_Windows_9.0.7652.zip`

Archive SHA-256:

- macOS 9.0.7652: `6576b1e6f061f0a5a6cad4a915b2ab7d43ae0378a331034bd33055cb74e20945`
- Windows 9.0.7652: `50deac884989d81d9246fc31d6a5e0ade76899ec64d05c7cf4ba477ddce0efd3`

Runtime defaults:

- macOS: `macos/ImSDKForMac_Plus.framework/Versions/A/ImSDKForMac_Plus`
- Windows x64: `windows/shared_lib/Win64/ImSDK.dll`
- Windows x86: `windows/shared_lib/Win32/ImSDK.dll`

`MULTI_AI_IM_SDK_LIBRARY` can still override the library path for local debugging.
