#!/usr/bin/env bash
set -euo pipefail

source_root="$1"
build_root="$2"
install_root="$3"
platform="$4"
compiler="$5"
arch="$6"
sysroot="$7"
archive_tool="$8"
ranlib_tool="$9"

mkdir -p "$build_root" "$install_root"
source_root="$(cd "$source_root" && pwd -P)"
build_root="$(cd "$build_root" && pwd -P)"
install_root="$(cd "$install_root" && pwd -P)"
meson_cross=""
compiler_flags="-O2 -fPIC"
cc_command="$compiler"
host=""
codec_asm=false
case "$arch" in
  arm64|aarch64|arm) codec_asm=true ;;
esac

if [[ "$platform" == iOS || "$platform" == Android ]]; then
  if [[ "$arch" == arm64 || "$arch" == aarch64 ]]; then
    cpu_family=aarch64
    host_cpu=aarch64
  elif [[ "$arch" == x86_64 ]]; then
    cpu_family=x86_64
    host_cpu=x86_64
  else
    cpu_family=arm
    host_cpu=arm
  fi
  if [[ "$platform" == iOS ]]; then
    target="$arch-apple-ios16.0"
    [[ "$sysroot" == *iPhoneSimulator* ]] && target="$target-simulator"
    host="$host_cpu-apple-darwin"
    cc_command+=" -target $target -isysroot $sysroot"
    system=darwin
  else
    host="$host_cpu-linux-android"
    cc_command+=" --sysroot=$sysroot"
    system=android
  fi
  cross_file="$build_root/meson-cross.ini"
  {
    printf '[binaries]\n'
    printf "c = '%s'\n" "$compiler"
    printf "ar = '%s'\n" "$archive_tool"
    printf '[host_machine]\n'
    printf "system = '%s'\n" "$system"
    printf "cpu_family = '%s'\n" "$cpu_family"
    printf "cpu = '%s'\n" "$host_cpu"
    printf "endian = 'little'\n"
    printf '[built-in options]\n'
    if [[ "$platform" == iOS ]]; then
      printf "c_args = ['-target', '%s', '-isysroot', '%s', '-fPIC']\n" "$target" "$sysroot"
      printf "c_link_args = ['-target', '%s', '-isysroot', '%s']\n" "$target" "$sysroot"
    else
      printf "c_args = ['--sysroot=%s', '-fPIC']\n" "$sysroot"
      printf "c_link_args = ['--sysroot=%s']\n" "$sysroot"
    fi
  } > "$cross_file"
  meson_cross="--cross-file=$cross_file"
fi

run_meson_setup() {
  if [[ -n "$meson_cross" ]]; then
    meson setup "$@" "$meson_cross"
  else
    meson setup "$@"
  fi
}

if [[ -f "$build_root/dav1d/build.ninja" ]]; then
  run_meson_setup "$build_root/dav1d" "$source_root/dav1d" --reconfigure \
    --prefix="$install_root" --libdir=lib --default-library=static \
    "-Denable_asm=$codec_asm" -Denable_tools=false -Denable_tests=false \
    -Denable_examples=false -Denable_docs=false \
    > "$build_root/dav1d-config.log" 2>&1
else
  run_meson_setup "$build_root/dav1d" "$source_root/dav1d" \
    --prefix="$install_root" --libdir=lib --default-library=static \
    "-Denable_asm=$codec_asm" -Denable_tools=false -Denable_tests=false \
    -Denable_examples=false -Denable_docs=false \
    > "$build_root/dav1d-config.log" 2>&1
fi
meson compile -C "$build_root/dav1d" -j6 > "$build_root/dav1d-build.log" 2>&1
meson install -C "$build_root/dav1d" > "$build_root/dav1d-install.log" 2>&1

mkdir -p "$build_root/x264"
if [[ "$codec_asm" == true && -f "$build_root/x264/config.mak" ]] &&
    grep -q '^AS=$' "$build_root/x264/config.mak"; then
  make -C "$build_root/x264" distclean > "$build_root/x264-clean.log" 2>&1
fi
if [[ ! -f "$build_root/x264/config.mak" ]]; then
  x264_asm_options=(--enable-pic)
  if [[ "$codec_asm" != true ]]; then x264_asm_options+=(--disable-asm); fi
  (cd "$build_root/x264" && CC="$cc_command" \
    AR="$archive_tool" RANLIB="$ranlib_tool" \
    "$source_root/x264/configure" "--prefix=$install_root" \
    "--libdir=$install_root/lib" --enable-static --disable-opencl \
    --disable-cli "${x264_asm_options[@]}" \
    ${host:+--host=$host}) > "$build_root/x264-config.log" 2>&1
fi
make -C "$build_root/x264" -j6 > "$build_root/x264-build.log" 2>&1
make -C "$build_root/x264" install-lib-static > "$build_root/x264-install.log" 2>&1

mkdir -p "$build_root/lame"
if [[ ! -f "$build_root/lame/Makefile" ]]; then
  (cd "$build_root/lame" && CC="$cc_command" \
    AR="$archive_tool" RANLIB="$ranlib_tool" CFLAGS="$compiler_flags" \
    "$source_root/lame/configure" "--prefix=$install_root" \
    "--libdir=$install_root/lib" --disable-shared --enable-static \
    --disable-frontend --disable-decoder \
    ${host:+--host=$host}) > "$build_root/lame-config.log" 2>&1
fi
make -C "$build_root/lame" -j6 > "$build_root/lame-build.log" 2>&1
make -C "$build_root/lame" install > "$build_root/lame-install.log" 2>&1
