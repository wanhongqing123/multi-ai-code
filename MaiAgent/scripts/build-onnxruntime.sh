#!/usr/bin/env bash
set -euo pipefail

# 从仓库内的官方 ONNX Runtime 源码构建，第三方依赖由其 CMake 清单固定版本。
# 构建目录必须在源码树之外，避免把平台产物混进待提交的源码。
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$(cd "$script_dir/../third_party/onnxruntime" && pwd)"
platform="${1:-}"
build_dir="${2:-}"
if [[ -z "$platform" || -z "$build_dir" ]]; then
  echo "Usage: $0 macos|ios|android <build-dir> [android-sdk] [android-ndk] [android-abi]" >&2
  exit 2
fi

# CMake 4 不接受部分上游依赖的旧 minimum 声明；强制使用 ORT 固定的
# Protobuf，避免 Homebrew 新版头文件与上游生成的 proto 文件混用。
common=(
  --config Release
  --build_dir "$build_dir"
  --update --build
  --parallel 6
  --skip_tests
  --skip_submodule_sync
  --compile_no_warning_as_error
  --cmake_extra_defines
  CMAKE_POLICY_VERSION_MINIMUM=3.5
  CMAKE_DISABLE_FIND_PACKAGE_Protobuf=ON
  onnxruntime_BUILD_UNIT_TESTS=OFF
)

case "$platform" in
  macos)
    # Core ML 面向 Apple 芯片，XNNPACK 提供移动 CPU 优化；CPU 是默认回退。
    "$source_dir/build.sh" "${common[@]}" --build_shared_lib --use_coreml --use_xnnpack
    runtime="$build_dir/Release/libonnxruntime.1.26.0.dylib"
    if [[ ! -f "$runtime" ]]; then
      echo "ONNX Runtime build did not produce $runtime" >&2
      exit 1
    fi
    stage_dir="$script_dir/../build/vendor/onnxruntime-macos"
    mkdir -p "$stage_dir"
    cp "$runtime" "$stage_dir/libonnxruntime.1.26.0.dylib"
    ;;
  ios)
    # iPhone 真机产物。模拟器需要单独以 iphonesimulator sysroot 构建并合并框架。
    exec "$source_dir/build.sh" "${common[@]}" --ios --cmake_generator Xcode \
      --osx_arch arm64 \
      --apple_sysroot iphoneos --apple_deploy_target 16.0 \
      --build_apple_framework --use_coreml --use_xnnpack
    ;;
  android)
    sdk_dir="${3:-}"
    ndk_dir="${4:-}"
    android_abi="${5:-arm64-v8a}"
    if [[ -z "$sdk_dir" || -z "$ndk_dir" ]]; then
      echo "Android requires the SDK and NDK directory paths" >&2
      exit 2
    fi
    case "$android_abi" in
      armeabi-v7a|arm64-v8a|x86|x86_64) ;;
      *) echo "Unsupported Android ABI: $android_abi" >&2; exit 2 ;;
    esac
    # NNAPI 使用设备 NPU/DSP，XNNPACK 优化 CPU；各 ABI 用不同构建目录。
    exec "$source_dir/build.sh" "${common[@]}" --android \
      --android_sdk_path "$sdk_dir" --android_ndk_path "$ndk_dir" \
      --android_abi "$android_abi" --android_api 26 --build_shared_lib \
      --use_nnapi --use_xnnpack
    ;;
  *)
    echo "Unsupported platform: $platform" >&2
    exit 2
    ;;
esac
