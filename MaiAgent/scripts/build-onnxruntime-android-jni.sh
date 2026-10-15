#!/usr/bin/env bash
# 文件作用：为 Android JNI 构建本地 ONNX Runtime，并保留与应用 ABI 一致的产物。
# 阅读边界：此文件负责构建或测试流程，不在应用运行时解释为业务逻辑。
set -euo pipefail

# Android 没有使用 ONNX Runtime 的 Java API，只打包由源码编出的原生 C API 库。
# 两个 ABI 与 App 的 abiFilters 一致；按需传 ABI 参数可单独重建其中一个。
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
agent_dir="$(cd "$script_dir/.." && pwd)"
sdk_dir="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ndk_dir="${ANDROID_NDK_HOME:-$sdk_dir/ndk/27.3.13750724}"
build_root="$agent_dir/build/vendor/onnxruntime-android-build"
output_root="$agent_dir/build/vendor/onnxruntime-android-jni"
abis=("$@")
if (( ${#abis[@]} == 0 )); then
  abis=(armeabi-v7a arm64-v8a)
fi

for abi in "${abis[@]}"; do
  "$script_dir/build-onnxruntime.sh" android "$build_root/$abi" \
    "$sdk_dir" "$ndk_dir" "$abi"
  runtime="$build_root/$abi/Release/libonnxruntime.so"
  if [[ ! -f "$runtime" ]]; then
    echo "ONNX Runtime build did not produce $runtime" >&2
    exit 1
  fi
  mkdir -p "$output_root/$abi"
  cp "$runtime" "$output_root/$abi/libonnxruntime.so"
done

echo "Local ONNX Runtime JNI libraries: $output_root"
