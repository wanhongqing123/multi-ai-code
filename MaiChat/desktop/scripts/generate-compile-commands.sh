#!/usr/bin/env bash
# 文件作用：用桌面端真实 CMake 配置生成 clangd 的 compile_commands.json。
# 编译数据库保留 Qt、MaiAgent 和系统 SDK 的实际选项；不手写包含路径或宏。
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${project_dir}/build"

cmake -S "${project_dir}" -B "${build_dir}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sfn build/compile_commands.json "${project_dir}/compile_commands.json"
python3 "${project_dir}/../scripts/generate-compile-commands.py" --platform desktop
printf 'clangd compilation database: %s\n' "${project_dir}/compile_commands.json"
