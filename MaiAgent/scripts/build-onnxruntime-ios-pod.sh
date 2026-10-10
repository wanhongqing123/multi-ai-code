#!/usr/bin/env bash
set -euo pipefail

# 用上游官方脚本从源码构建 iPhone+模拟器 XCFramework，再组装本地 CocoaPods。
# App 的 Podfile 固定引用此本地 Pod，不再回退下载另一份预编译运行库。
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
agent_dir="$(cd "$script_dir/.." && pwd)"
source_dir="$agent_dir/third_party/onnxruntime"
build_dir="${1:-$agent_dir/build/vendor/onnxruntime-ios-build}"
staging_dir="$agent_dir/build/vendor/onnxruntime-ios-pods"

python3 "$source_dir/tools/ci_build/github/apple/build_and_assemble_apple_pods.py" \
  --build-dir "$build_dir" \
  --staging-dir "$staging_dir" \
  --build-settings-file "$script_dir/onnxruntime-ios-build-settings.json" \
  --build-apple-framework-arg=--build_dynamic_framework

test -f "$staging_dir/onnxruntime-c/onnxruntime-c.podspec"
echo "Local ONNX Runtime Pod: $staging_dir/onnxruntime-c"
