#!/usr/bin/env bash
set -euo pipefail

source_dir="$1"
build_dir="$2"
architecture="$3"
compiler="$4"

mkdir -p "$build_dir"
cd "$build_dir"

if ! "$source_dir/configure" \
  --disable-everything --disable-programs --disable-doc --disable-debug \
  --disable-autodetect --disable-asm --disable-gpl --disable-nonfree \
  --enable-pic --enable-static --disable-shared \
  --target-os=darwin "--arch=$architecture" "--cc=$compiler" \
  > configure.log 2>&1; then
  tail -n 80 configure.log >&2
  exit 1
fi
