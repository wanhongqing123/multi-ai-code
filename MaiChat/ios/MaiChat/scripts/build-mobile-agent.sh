#!/bin/bash
# 文件作用：按 iOS 设备或模拟器配置构建移动端 Agent C++ 库。
# 阅读边界：此文件负责构建或测试流程，不在应用运行时解释为业务逻辑。
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
repo_dir="$(cd "$PROJECT_DIR/../../.." && pwd)"
# OpenSSL、FFmpeg、OpenCV 和 MaiAgent 共用稳定的分平台构建目录。
# Xcode 切换或清理 DerivedData 时，只重做最终归档和 App 链接，不重编第三方源码。
agent_arch="${ARCHS// /-}"
# 隔离验收可指定独立构建目录，避免覆盖正在使用的产品构建缓存。
agent_build="${MAICHAT_MOBILE_AGENT_BUILD_DIR:-$repo_dir/MaiAgent/build/vendor/mobile-agent-$PLATFORM_NAME-$agent_arch}"
cmake -S "$repo_dir/MaiChat/MobileAgentAdapter" -B "$agent_build" -G Ninja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_ARCHITECTURES="${ARCHS// /;}" -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake --build "$agent_build" --target maichat_mobile --parallel 6
# The Swift C bridge calls this archive directly; ensure the dependency exists
# before the complete set of static libraries is merged into the App archive.
test -f "$agent_build/mai_network/libmai_screen_frame_archive.a"
cmake --build "$agent_build" --target maichat_ffplay_hosted --parallel 6
hosted_source="$agent_build/libmaichat_ffplay_hosted.dylib"
# OTHER_LDFLAGS 使用 DERIVED_FILE_DIR 下的链接路径；只更新 App Frameworks
# 会让链接器读到上次构建的旧 dylib，新增 FFplay C 接口时出现假性未定义符号。
hosted_link_dir="$DERIVED_FILE_DIR/MobileAgentAdapter-${PLATFORM_NAME}-graphics"
mkdir -p "$hosted_link_dir"
cp "$hosted_source" "$hosted_link_dir/libmaichat_ffplay_hosted.dylib"
archive_list=()
# 独立核心配置可能留下后备 JPEG 归档；产品已有 OpenCV JPEG 时不把旧后备再合入，
# 防止一个 App archive 携带两份同名 libjpeg 实现。qpdf 的 CMake 链接也只选择这一份。
has_opencv_jpeg=false
if find "$agent_build" -name 'liblibjpeg-turbo.a' -type f -print -quit | /usr/bin/grep -q .; then
  has_opencv_jpeg=true
fi
while IFS= read -r archive; do archive_list+=("$archive"); done < <(
  find "$agent_build" -name '*.a' -type f \
    ! -path '*/vendor/ffmpeg-build/install/*' \
    ! -path '*/vendor/ffmpeg-build/vendor-build/*' \
    ! -path '*/vendor/ffmpeg-build/vendor-install/*' \
    ! -path '*/vendor/quic-build/*' \
    | while IFS= read -r archive; do
        if [[ "$has_opencv_jpeg" == true && "$archive" == */libmai_pdf_jpeg_fallback.a ]]; then continue; fi
        printf '%s\n' "$archive"
      done
)
# 根据当前构建配置显式加入 QUIC 库；关闭后不把旧缓存归档带进 App。
if /usr/bin/grep -q '^MAI_ENABLE_QUIC:BOOL=ON$' "$agent_build/CMakeCache.txt"; then
  quic_lib_dir="$agent_build/maiagent/vendor/quic-build/install/lib"
  for library in libngtcp2.a libngtcp2_crypto_ossl.a libnghttp3.a; do
    test -f "$quic_lib_dir/$library"
    archive_list+=("$quic_lib_dir/$library")
  done
fi
for codec in libdav1d.a libx264.a libmp3lame.a; do
  archive_list+=("$agent_build/vendor/ffmpeg-build/vendor-install/lib/$codec")
done
xcrun libtool -static -o "$DERIVED_FILE_DIR/libMaiMobileAgent.a" "${archive_list[@]}"
if [[ -n "${TARGET_BUILD_DIR:-}" && -n "${FRAMEWORKS_FOLDER_PATH:-}" ]]; then
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
  # PDF 引擎以静态库进入 App，同时随包保留其许可与署名。
  pdf_licenses="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/ThirdPartyLicenses"
  mkdir -p "$pdf_licenses"
  cp "$repo_dir/MaiAgent/third_party/qpdf/LICENSE.txt" "$pdf_licenses/QPDF_LICENSE.txt"
  cp "$repo_dir/MaiAgent/third_party/qpdf/NOTICE.md" "$pdf_licenses/QPDF_NOTICE.md"
  # QUIC 和 HTTP/3 库静态链接进 App，随包保留上游 MIT 许可证。
  cp "$repo_dir/MaiAgent/third_party/ngtcp2/COPYING" "$pdf_licenses/NGTCP2_LICENSE.txt"
  cp "$repo_dir/MaiAgent/third_party/nghttp3/COPYING" "$pdf_licenses/NGHTTP3_LICENSE.txt"
  cp "$repo_dir/MaiAgent/third_party/curl-apple/COPYING" "$pdf_licenses/CURL_LICENSE.txt"
  effect_bundle="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/MaiAgentGraphics"
  mkdir -p "$effect_bundle"
  cp "$repo_dir/MaiAgent/Graphics/data/"*.effect "$effect_bundle/"
  cp "$repo_dir/MaiChat/Media/data/"*.effect "$effect_bundle/"
  model_bundle="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/MaiAgentModels"
  mkdir -p "$model_bundle"
  cp "$repo_dir/MaiAgent/models/rvm_mobilenetv3_fp32.onnx" "$model_bundle/"
  cp "$repo_dir/MaiAgent/models/RVM_LICENSE" "$model_bundle/"
  # LaMa 用于水印与人物轮廓重叠时的局部修补；模型从发布源手动下载并校验。
  # 缺文件就让构建明确失败，不能静默交付一个没有去水印工具的 iOS 包。
  if [[ ! -f "$repo_dir/MaiAgent/models/lama_fp32.onnx" ]]; then
    echo "Missing LaMa model. Run MaiAgent/scripts/fetch-lama-model.sh first." >&2
    exit 1
  fi
  cp "$repo_dir/MaiAgent/models/lama_fp32.onnx" "$model_bundle/"
  cp "$repo_dir/MaiAgent/models/LaMa_LICENSE" "$model_bundle/"
  # iOS 的 OpenSSL libcurl 需要随 App 打包的受信任 CA，不能关闭证书校验。
  cert_bundle="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/MaiAgentCertificates"
  mkdir -p "$cert_bundle"
  cp "$repo_dir/MaiAgent/certs/cacert.pem" "$cert_bundle/"
fi
