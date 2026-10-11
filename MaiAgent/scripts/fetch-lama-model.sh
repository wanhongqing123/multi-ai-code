#!/bin/bash
set -euo pipefail

# 只在需要本地去水印模型时手动获取；常规编译不访问网络。
script_dir="$(cd "$(dirname "$0")" && pwd)"
model_dir="$(cd "$script_dir/../models" && pwd)"
target="$model_dir/lama_fp32.onnx"
expected="1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6"
if [[ -f "$target" ]]; then
  actual="$(shasum -a 256 "$target" | awk '{print $1}')"
  if [[ "$actual" == "$expected" ]]; then
    printf 'LaMa model is ready: %s\n' "$target"
    exit 0
  fi
  printf 'Existing LaMa model has an unexpected checksum: %s\n' "$target" >&2
  exit 1
fi
temporary="$target.partial.$$"
trap 'rm -f "$temporary"' EXIT
curl --fail --location --show-error --output "$temporary" \
  'https://huggingface.co/Carve/LaMa-ONNX/resolve/main/lama_fp32.onnx'
actual="$(shasum -a 256 "$temporary" | awk '{print $1}')"
if [[ "$actual" != "$expected" ]]; then
  printf 'Downloaded LaMa model checksum mismatch\n' >&2
  exit 1
fi
mv "$temporary" "$target"
printf 'LaMa model is ready: %s\n' "$target"
