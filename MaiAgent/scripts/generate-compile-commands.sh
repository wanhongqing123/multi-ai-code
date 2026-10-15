#!/usr/bin/env bash
# 文件作用：为 MaiAgent 当前平台生成 clangd 使用的 compile_commands.json。
# 编译数据库由 CMake 根据真实构建选项生成，避免手写参数与工程脱节。
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${project_dir}/build"

if [[ -f "${build_dir}/CMakeCache.txt" ]]; then
    cmake -S "${project_dir}" -B "${build_dir}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
else
    cmake -S "${project_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
fi

ln -sfn build/compile_commands.json "${project_dir}/compile_commands.json"
printf 'clangd compilation database: %s\n' "${project_dir}/compile_commands.json"
