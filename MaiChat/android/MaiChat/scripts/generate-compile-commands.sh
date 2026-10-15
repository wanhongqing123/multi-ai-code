#!/usr/bin/env bash
# 文件作用：把 Android Gradle 原生构建产生的真实编译数据库暴露给 clangd。
# 数据库包含当前 arm64-v8a 的 NDK、JNI 和 MaiAgent 选项；不手写编译命令。
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "${script_dir}/.." && pwd)"
database="${project_dir}/app/.cxx/mobile-agent-adapter/tools/debug/arm64-v8a/compile_commands.json"

if [[ ! -f "${database}" ]]; then
    printf 'Android native compilation database is missing. Build the app Debug variant first.\n' >&2
    exit 1
fi

ln -sfn .cxx/mobile-agent-adapter/tools/debug/arm64-v8a/compile_commands.json \
    "${project_dir}/app/compile_commands.json"
printf 'clangd compilation database: %s\n' "${project_dir}/app/compile_commands.json"
