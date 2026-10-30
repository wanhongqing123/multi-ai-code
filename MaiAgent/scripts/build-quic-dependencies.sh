#!/usr/bin/env bash
set -euo pipefail

# 离线构建固定版本的 QUIC 依赖。调用方提供项目 OpenSSL 的生成目录和源码目录，
# 避免意外链接本机包管理器中的另一份 OpenSSL；输出目录必须由调用方单独管理。
if [[ $# -ne 8 ]]; then
  printf 'Usage: %s <source-root> <build-root> <install-root> <openssl-build> <openssl-source> <sdk> <architecture> <deployment-target>\n' "$0" >&2
  exit 2
fi

source_root=$1
build_root=$2
install_root=$3
openssl_build=$4
openssl_source=$5
sdk=$6
architecture=$7
deployment_target=$8

# 与 App 采用同一个 SDK 和架构，避免把 Mac 的库误链接进 iOS 包。
case "$sdk" in
  macosx) system_name=Darwin ;;
  iphoneos|iphonesimulator) system_name=iOS ;;
  *) printf 'Unsupported Apple SDK: %s\n' "$sdk" >&2; exit 2 ;;
esac
sdk_path=$(xcrun --sdk "$sdk" --show-sdk-path)
platform_args=(
  -DCMAKE_SYSTEM_NAME="$system_name"
  -DCMAKE_OSX_SYSROOT="$sdk_path"
  -DCMAKE_OSX_ARCHITECTURES="$architecture"
)
if [[ -n "$deployment_target" ]]; then
  platform_args+=(-DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target")
elif [[ "$system_name" == iOS ]]; then
  printf 'An iOS deployment target is required\n' >&2
  exit 2
fi

for required in \
  "$source_root/nghttp3/CMakeLists.txt" \
  "$source_root/ngtcp2/CMakeLists.txt" \
  "$openssl_build/include/openssl/opensslv.h" \
  "$openssl_build/libssl.a" \
  "$openssl_build/libcrypto.a" \
  "$openssl_source/include/openssl/opensslconf.h"; do
  if [[ ! -f "$required" ]]; then
    printf 'Required QUIC build input is missing: %s\n' "$required" >&2
    exit 1
  fi
done

# OpenSSL 的配置头由构建生成，其余头来自上游源码；两个目录缺一不可。
cmake -S "$source_root/nghttp3" -B "$build_root/nghttp3" \
  -DCMAKE_BUILD_TYPE=Release "${platform_args[@]}" \
  -DCMAKE_INSTALL_PREFIX="$install_root" -DENABLE_LIB_ONLY=ON \
  -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=OFF -DBUILD_TESTING=OFF
cmake --build "$build_root/nghttp3" --target install --parallel 4

cmake -S "$source_root/ngtcp2" -B "$build_root/ngtcp2" \
  -DCMAKE_BUILD_TYPE=Release "${platform_args[@]}" \
  -DCMAKE_INSTALL_PREFIX="$install_root" -DENABLE_LIB_ONLY=ON \
  -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=OFF -DBUILD_TESTING=OFF \
  -DENABLE_OPENSSL=ON -DOPENSSL_USE_STATIC_LIBS=ON \
  -DOPENSSL_INCLUDE_DIR="$openssl_build/include" \
  -DOPENSSL_SSL_LIBRARY="$openssl_build/libssl.a" \
  -DOPENSSL_CRYPTO_LIBRARY="$openssl_build/libcrypto.a" \
  -DCMAKE_C_FLAGS="-I\"$openssl_source/include\""
cmake --build "$build_root/ngtcp2" --target install --parallel 4

# 缺少任一静态库意味着 TLS 或协议后端没有按预期生成，禁止继续链接产品。
for archive in libnghttp3.a libngtcp2.a libngtcp2_crypto_ossl.a; do
  if [[ ! -f "$install_root/lib/$archive" ]]; then
    printf 'Expected QUIC archive is missing: %s\n' "$archive" >&2
    exit 1
  fi
done
