#!/usr/bin/env bash
set -euo pipefail

source_dir="$1"
build_dir="$2"
platform="$3"
arch="$4"
sysroot="$5"
ndk_root="$6"
archive_tool="${AR:-ar}"

mkdir -p "$build_dir"
if [[ -f "$build_dir/libcrypto.a" && -f "$build_dir/libmai_openssl_cli.a" ]]; then
  cli_size=$(wc -c < "$build_dir/libmai_openssl_cli.a")
  if ((cli_size > 4096)); then
    exit 0
  fi
  rm "$build_dir/libmai_openssl_cli.a"
fi

if [[ "$platform" == iOS ]]; then
  if [[ "$sysroot" == *iPhoneSimulator* ]]; then
    if [[ "$arch" == arm64 ]]; then
      target=iossimulator-arm64-xcrun
    elif [[ "$arch" == x86_64 ]]; then
      target=iossimulator-x86_64-xcrun
    else
      echo "Unsupported iOS simulator architecture: $arch" >&2
      exit 1
    fi
    min_version=-mios-simulator-version-min=16.0
  elif [[ "$arch" == arm64 ]]; then
    target=ios64-xcrun
    min_version=-miphoneos-version-min=16.0
  else
    echo "Unsupported iOS device architecture: $arch" >&2
    exit 1
  fi
elif [[ "$platform" == macOS ]]; then
  if [[ "$arch" != arm64 && "$arch" != x86_64 ]]; then
    echo "Unsupported macOS architecture: $arch" >&2
    exit 1
  fi
  target="darwin64-${arch}-cc"
  min_version=-mmacosx-version-min=13.0
elif [[ "$platform" == Android ]]; then
  if [[ -z "$ndk_root" ]]; then
    echo "Android NDK path is required" >&2
    exit 1
  fi
  export ANDROID_NDK_ROOT="$ndk_root"
  if [[ -d "$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin" ]]; then
    export PATH="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH"
    archive_tool="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ar"
  elif [[ -d "$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin" ]]; then
    export PATH="$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
    archive_tool="$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar"
  else
    echo "Android NDK compiler toolchain was not found" >&2
    exit 1
  fi
  if [[ "$arch" == arm64-v8a ]]; then
    target=android-arm64
  elif [[ "$arch" == armeabi-v7a ]]; then
    target=android-arm
  else
    echo "Unsupported Android ABI: $arch" >&2
    exit 1
  fi
  min_version=-D__ANDROID_API__=26
else
  echo "Unsupported OpenSSL platform: $platform" >&2
  exit 1
fi

cd "$build_dir"
if [[ ! -f Makefile ]]; then
  perl "$source_dir/Configure" "$target" "$min_version" -fPIC -DMAI_OPENSSL_EMBEDDED \
    no-shared no-tests no-module no-async no-legacy no-comp \
    --prefix="$build_dir/install"
fi
make -j 6 build_generated
make -j 6 apps/openssl

cli_objects=()
for object in apps/openssl-bin-*.o apps/lib/openssl-bin-cmp_mock_srv.o; do
  [[ -f "$object" ]] || continue
  [[ "$object" == *mai_openssl_stub_main.o ]] && continue
  cli_objects+=("$object")
done
if [[ "${#cli_objects[@]}" -lt 3 || ! -f apps/libapps.a || ! -f libssl.a ]]; then
  echo "OpenSSL CLI objects or supporting archives are missing" >&2
  exit 1
fi
"$archive_tool" rcs libmai_openssl_cli.a "${cli_objects[@]}"
