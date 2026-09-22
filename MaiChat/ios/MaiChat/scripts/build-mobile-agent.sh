#!/bin/bash
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
repo_dir="$(cd "$PROJECT_DIR/../../.." && pwd)"
agent_build="$DERIVED_FILE_DIR/MaiAgent-$PLATFORM_NAME"
cmake -S "$repo_dir/MaiChat/shared/agent" -B "$agent_build" \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_ARCHITECTURES="${ARCHS// /;}" -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake --build "$agent_build" --target maichat_mobile --parallel 6
archive_list=()
while IFS= read -r archive; do archive_list+=("$archive"); done < <(find "$agent_build" -name '*.a' -type f)
xcrun libtool -static -o "$DERIVED_FILE_DIR/libMaiMobileAgent.a" "${archive_list[@]}"
