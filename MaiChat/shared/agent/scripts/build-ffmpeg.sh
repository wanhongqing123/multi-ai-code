#!/usr/bin/env bash
set -euo pipefail

source_dir="$1"
build_dir="$2"
platform="$3"
compiler="$4"
arch="$5"
sysroot="$6"
archive_tool="$7"
ranlib_tool="$8"

mkdir -p "$build_dir"
cd "$build_dir"

options=(
  "--prefix=$build_dir/install"
  --disable-programs --disable-doc --disable-everything
  --disable-avcodec --disable-avformat --disable-avdevice --disable-swresample
  --disable-autodetect --disable-network --disable-asm --disable-debug
  --disable-gpl --disable-nonfree
  --enable-avfilter --enable-swscale
  --enable-filter=crop,scale,hflip,vflip,hue,format,transpose,lutrgb,gblur
  --enable-pic --enable-static --disable-shared
  "--cc=$compiler" "--ar=$archive_tool" "--ranlib=$ranlib_tool"
)

case "$platform" in
  Android)
    options+=(--target-os=android "--arch=$arch" --enable-cross-compile "--sysroot=$sysroot")
    ;;
  iOS)
    target="$arch-apple-ios16.0"
    if [[ "$sysroot" == *iPhoneSimulator* ]]; then
      target="$arch-apple-ios16.0-simulator"
    fi
    options+=(--target-os=darwin "--arch=$arch" --enable-cross-compile
      "--sysroot=$sysroot" "--extra-cflags=-target $target"
      "--extra-ldflags=-target $target")
    ;;
  Darwin)
    options+=(--target-os=darwin "--arch=$arch")
    ;;
  *)
    echo "Unsupported FFmpeg build platform: $platform" >&2
    exit 1
    ;;
esac

"$source_dir/configure" "${options[@]}"
make -j6
make install
