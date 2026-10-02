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

vendor_prefix="$build_dir/vendor-install"
vendor_build="$build_dir/vendor-build"
bash "$source_dir/../build-media-codecs.sh" "$source_dir/.." \
  "$vendor_build" "$vendor_prefix" Darwin "$compiler" "$architecture" \
  "" "$archive_tool" "$ranlib_tool"

export PKG_CONFIG_LIBDIR="$vendor_prefix/lib/pkgconfig"
export PKG_CONFIG_PATH=

if ! "$source_dir/configure" \
  "--prefix=$build_dir/install" \
  --disable-programs --disable-doc --disable-debug \
  --disable-autodetect --enable-zlib \
  --enable-audiotoolbox --enable-videotoolbox \
  --enable-gpl --disable-nonfree \
  --enable-libdav1d --enable-libx264 --enable-libmp3lame \
  --pkg-config-flags=--static \
  "--extra-cflags=-I$vendor_prefix/include" \
  "--extra-ldflags=-L$vendor_prefix/lib" \
  --enable-pic --enable-static --disable-shared \
  --target-os=darwin "--arch=$architecture" \
  "--cc=$compiler" "--ar=$archive_tool" "--ranlib=$ranlib_tool" \
  > configure.log 2>&1; then
  tail -n 80 configure.log >&2
  exit 1
fi

rm -f libmaifftools.a fftools/ffmpeg.o fftools/ffprobe.o \
  fftools/cmdutils.o fftools/opt_common.o
make -j6 > build.log 2>&1 || { tail -n 80 build.log >&2; exit 1; }
make -j6 libmaifftools.a >> build.log 2>&1 || { tail -n 80 build.log >&2; exit 1; }
make install > install.log 2>&1 || { tail -n 80 install.log >&2; exit 1; }
