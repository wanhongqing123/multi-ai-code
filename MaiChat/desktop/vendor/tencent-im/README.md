# Tencent IM Desktop SDK

This directory contains the native desktop IM SDK used by the standalone Qt IM app.

Sources:

- macOS: `https://im.sdk.qcloud.com/download/plus/9.0.7652/ImSDKForMac_Plus_9.0.7652.framework.zip`
- Windows: `https://im.sdk.qcloud.com/download/plus/8.9.7511/cross_platform/ImSDK_Windows_8.9.7511.zip`

Runtime defaults:

- macOS: `macos/ImSDKForMac_Plus.framework/Versions/A/ImSDKForMac_Plus`
- Windows x64: `windows/shared_lib/Win64/ImSDK.dll`
- Windows x86: `windows/shared_lib/Win32/ImSDK.dll`

`MULTI_AI_IM_SDK_LIBRARY` can still override the library path for local debugging.
