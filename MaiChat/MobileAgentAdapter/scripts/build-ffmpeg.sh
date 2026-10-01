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
  --disable-programs --disable-doc --disable-debug
  --disable-autodetect --disable-asm
  --disable-audiotoolbox --disable-videotoolbox
  --disable-gpl --disable-nonfree
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
    host_sdk="$(xcrun --sdk macosx --show-sdk-path)"
    host_target="$(uname -m)-apple-macos13.0"
    options+=(--target-os=darwin "--arch=$arch" --enable-cross-compile
      "--sysroot=$sysroot" "--extra-cflags=-target $target"
      "--extra-ldflags=-target $target"
      --host-cc=/usr/bin/clang --host-ld=/usr/bin/clang
      "--host-cflags=-target $host_target -isysroot $host_sdk"
      "--host-ldflags=-target $host_target -isysroot $host_sdk")
    ;;
  Darwin)
    options+=(--target-os=darwin "--arch=$arch")
    ;;
  *)
    echo "Unsupported FFmpeg build platform: $platform" >&2
    exit 1
    ;;
esac

if [[ "$platform" == iOS ]]; then
  # Xcode exports the iPhone SDKROOT to child tools. FFmpeg's bin2c runs on the
  # build Mac, so that SDKROOT would incorrectly produce a simulator executable.
  unset SDKROOT IPHONEOS_DEPLOYMENT_TARGET
  rm -f ffbuild/bin2c_host.o ffbuild/bin2c
fi

"$source_dir/configure" "${options[@]}"
make -j6
make -j6 libmaifftools.a
make install
