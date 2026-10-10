# ONNX Runtime 官方源码

- 上游：<https://github.com/microsoft/onnxruntime>
- 版本：`v1.26.0`，与 MaiChat 现有 C API 头、iOS Pod 和 Android 依赖版本一致。
- 来源：<https://github.com/microsoft/onnxruntime/archive/refs/tags/v1.26.0.tar.gz>
- 发布源码包 SHA-256：`2a90eb9a306c1eeb29213f5b165a55008ac5cb7d27e0935c4458c51a49ef091d`
- 许可：见本目录 `LICENSE`（MIT）及 `ThirdPartyNotices.txt`。

`third_party` 只保留本目录这一份 ONNX Runtime 源码。此目录由 GitHub 官方
发布源码包解压得到，没有 `.git` 或 Git 子模块。
为避免第三方代理指令进入主仓库，解压时去掉了上游的 `.agents`、`.claude`、
`.github` 和 `AGENTS.md` 等开发环境文件；运行库源码和官方构建脚本保留。

ONNX Runtime 是执行 `.onnx` 模型的引擎。本项目的 RVM 视频抠像使用
`MaiRvmMatting.cpp` 通过它的 C API 创建会话并逐帧推理。它的 CMake 依赖清单
固定引用 ONNX `v1.21.0`，构建时由上游脚本下载到构建缓存；主仓库不再单独
保存另一份 `onnx/onnx` 源码。

平台加速能力要分别编译并在创建推理会话时注册：Apple 用 Core ML 和
XNNPACK，Android 用 NNAPI、XNNPACK（可选 WebGPU），Windows 用 DirectML。
CPU 始终作为回退。CUDA、QNN 等后端还依赖对应厂商硬件和 SDK，不能在每个
平台无条件开启。首次构建会按上游锁定的版本下载依赖；普通 MaiAgent 构建
仍只需 C API 头，不触发源码构建。App 使用 `MaiAgent/build/vendor/` 下从本源码
编出的运行库，缺少产物时明确要求先运行对应平台构建脚本。

`MaiAgent/scripts/build-onnxruntime.sh` 提供单架构构建入口。iOS App 使用
`build-onnxruntime-ios-pod.sh` 生成本地 XCFramework/Pod，Android App 使用
`build-onnxruntime-android-jni.sh` 生成两个 ABI 的原生库。Windows x64 使用
`build-onnxruntime-windows.ps1`，启用 DirectML 和 XNNPACK。
构建目录位于源码树外，产物按平台保存在 `MaiAgent/build/vendor/`。首次准备：

```sh
MaiAgent/scripts/build-onnxruntime.sh macos MaiAgent/build/vendor/onnxruntime-macos-build
MaiAgent/scripts/build-onnxruntime-ios-pod.sh
MaiAgent/scripts/build-onnxruntime-android-jni.sh
```

macOS arm64 的源码构建已启用 Core ML 和 XNNPACK，并以仓库内的
`rvm_mobilenetv3_fp32.onnx` 跑通 `MaiRvmMattingTests` 的真实推理。iPhone 真机
和模拟器的 XCFramework 已编出，真机 App 已用本地 Pod 构建、安装并启动。
Android armv7/arm64 的源码库已打入 APK；模拟器上的真实视频抠像测试通过。
编入执行后端并不等于自动使用该后端；当前 RVM 会话尚未显式选择 Core ML、
NNAPI 或 XNNPACK，仍由 CPU 执行推理。

iOS 的 `build-mobile-agent.sh` 把 OpenSSL、FFmpeg、OpenCV 和 MaiAgent 的原生
构建缓存固定在 `MaiAgent/build/vendor/mobile-agent-<平台>-<架构>/`。切换或清理
Xcode DerivedData 后会复用这些库；平台、架构不同则分别构建。三方源码或
构建选项变化时应清理对应平台缓存再重建。
