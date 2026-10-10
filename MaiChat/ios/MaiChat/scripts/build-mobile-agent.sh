#!/bin/bash
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
repo_dir="$(cd "$PROJECT_DIR/../../.." && pwd)"
# OpenSSL、FFmpeg、OpenCV 和 MaiAgent 共用稳定的分平台构建目录。
# Xcode 切换或清理 DerivedData 时，只重做最终归档和 App 链接，不重编第三方源码。
agent_arch="${ARCHS// /-}"
agent_build="$repo_dir/MaiAgent/build/vendor/mobile-agent-$PLATFORM_NAME-$agent_arch"
cmake -S "$repo_dir/MaiChat/MobileAgentAdapter" -B "$agent_build" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_ARCHITECTURES="${ARCHS// /;}" -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake --build "$agent_build" --target maichat_mobile --parallel 6
cmake --build "$agent_build" --target maichat_ffplay_hosted --parallel 6
archive_list=()
while IFS= read -r archive; do archive_list+=("$archive"); done < <(
  find "$agent_build" -name '*.a' -type f \
    ! -path '*/vendor/ffmpeg-build/install/*' \
    ! -path '*/vendor/ffmpeg-build/vendor-build/*' \
    ! -path '*/vendor/ffmpeg-build/vendor-install/*'
)
for codec in libdav1d.a libx264.a libmp3lame.a; do
  archive_list+=("$agent_build/vendor/ffmpeg-build/vendor-install/lib/$codec")
done
xcrun libtool -static -o "$DERIVED_FILE_DIR/libMaiMobileAgent.a" "${archive_list[@]}"
if [[ -n "${TARGET_BUILD_DIR:-}" && -n "${FRAMEWORKS_FOLDER_PATH:-}" ]]; then
  hosted_source="$agent_build/libmaichat_ffplay_hosted.dylib"
  hosted_target="$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH/libmaichat_ffplay_hosted.dylib"
  mkdir -p "$(dirname "$hosted_target")"
  cp "$hosted_source" "$hosted_target"
  if [[ "${CODE_SIGNING_ALLOWED:-YES}" != NO ]]; then
    identity="${EXPANDED_CODE_SIGN_IDENTITY:-}"
    if [[ -n "$identity" ]]; then
      codesign --force --sign "$identity" "$hosted_target"
    fi
  fi
fi
if [[ -n "${TARGET_BUILD_DIR:-}" && -n "${UNLOCALIZED_RESOURCES_FOLDER_PATH:-}" ]]; then
  effect_bundle="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/MaiAgentGraphics"
  mkdir -p "$effect_bundle"
  cp "$repo_dir/MaiAgent/Graphics/data/"*.effect "$effect_bundle/"
  cp "$repo_dir/MaiChat/Media/data/"*.effect "$effect_bundle/"
  model_bundle="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/MaiAgentModels"
  mkdir -p "$model_bundle"
  cp "$repo_dir/MaiAgent/models/rvm_mobilenetv3_fp32.onnx" "$model_bundle/"
  cp "$repo_dir/MaiAgent/models/RVM_LICENSE" "$model_bundle/"
  # iOS 的 OpenSSL libcurl 需要随 App 打包的受信任 CA，不能关闭证书校验。
  cert_bundle="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/MaiAgentCertificates"
  mkdir -p "$cert_bundle"
  cp "$repo_dir/MaiAgent/certs/cacert.pem" "$cert_bundle/"
fi
