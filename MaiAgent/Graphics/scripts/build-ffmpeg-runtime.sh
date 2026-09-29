#!/usr/bin/env bash
set -euo pipefail

source_dir="$1"
build_dir="$2"
compiler="$3"
architecture="$4"
archive_tool="$5"
ranlib_tool="$6"

mkdir -p "$build_dir"
cd "$build_dir"

if ! "$source_dir/configure" \
  "--prefix=$build_dir/install" \
  --disable-programs --disable-doc --disable-debug \
  --disable-autodetect --disable-asm \
  --enable-audiotoolbox --enable-videotoolbox \
  --disable-gpl --disable-nonfree \
  --enable-pic --enable-static --disable-shared \
  --target-os=darwin "--arch=$architecture" \
  "--cc=$compiler" "--ar=$archive_tool" "--ranlib=$ranlib_tool" \
  > configure.log 2>&1; then
  tail -n 80 configure.log >&2
  exit 1
fi

make -j6 > build.log 2>&1 || { tail -n 80 build.log >&2; exit 1; }
make install > install.log 2>&1 || { tail -n 80 install.log >&2; exit 1; }
