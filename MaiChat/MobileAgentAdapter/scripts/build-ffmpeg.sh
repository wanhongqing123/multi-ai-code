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

# FFmpeg configure preserves existing Makefile/src links in a reused build
# directory. Replace links left by an older vendored source checkout.
if [[ -L src ]] && [[ "$(cd src && pwd -P)" != "$(cd "$source_dir" && pwd -P)" ]]; then
  rm -f src Makefile
fi

options=(
  "--prefix=$build_dir/install"
  --disable-programs --disable-doc --disable-debug
  --disable-autodetect --enable-zlib
  --disable-audiotoolbox
  --enable-gpl --disable-nonfree
  --enable-pic --enable-static --disable-shared
  "--cc=$compiler" "--ar=$archive_tool" "--ranlib=$ranlib_tool"
)

case "$platform" in
  Android)
    options+=(--target-os=android "--arch=$arch" --enable-cross-compile "--sysroot=$sysroot"
      "--extra-cflags=-fvisibility=hidden")
    ;;
  iOS)
    options+=(--enable-network --enable-securetransport --enable-videotoolbox
      --enable-protocol=http --enable-protocol=https
      --enable-protocol=tcp --enable-protocol=tls)
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

vendor_prefix="$build_dir/vendor-install"
bash "$source_dir/../build-media-codecs.sh" "$source_dir/.." \
  "$build_dir/vendor-build" "$vendor_prefix" "$platform" "$compiler" \
  "$arch" "$sysroot" "$archive_tool" "$ranlib_tool"
export PKG_CONFIG_LIBDIR="$vendor_prefix/lib/pkgconfig"
export PKG_CONFIG_PATH=
options+=(--enable-libdav1d --enable-libx264 --enable-libmp3lame
  --pkg-config-flags=--static
  "--extra-cflags=-I$vendor_prefix/include"
  "--extra-ldflags=-L$vendor_prefix/lib")

"$source_dir/configure" "${options[@]}"
rm -f libmaifftools.a fftools/ffmpeg.o fftools/ffprobe.o \
  fftools/cmdutils.o fftools/opt_common.o
make -j6
make -j6 libmaifftools.a
make install
