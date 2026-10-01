#!/bin/bash
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
repo_dir="$(cd "$PROJECT_DIR/../../.." && pwd)"
agent_build="$DERIVED_FILE_DIR/MobileAgentAdapter-$PLATFORM_NAME-graphics"
cmake -S "$repo_dir/MaiChat/MobileAgentAdapter" -B "$agent_build" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_ARCHITECTURES="${ARCHS// /;}" -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake --build "$agent_build" --target maichat_mobile --parallel 6
archive_list=()
while IFS= read -r archive; do archive_list+=("$archive"); done < <(
  find "$agent_build" -name '*.a' -type f ! -path '*/vendor/ffmpeg-build/install/*'
)
xcrun libtool -static -o "$DERIVED_FILE_DIR/libMaiMobileAgent.a" "${archive_list[@]}"
if [[ -n "${TARGET_BUILD_DIR:-}" && -n "${UNLOCALIZED_RESOURCES_FOLDER_PATH:-}" ]]; then
  effect_bundle="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/MaiAgentGraphics"
  mkdir -p "$effect_bundle"
  cp "$repo_dir/MaiAgent/Graphics/data/"*.effect "$effect_bundle/"
  cp "$repo_dir/MaiChat/Media/data/"*.effect "$effect_bundle/"
fi
