#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Package the Qt Desktop IM app for macOS.

Usage:
  MaiChat/desktop/scripts/package-macos.sh [--build-dir build] [--out-dir dist] [--macdeployqt /path/to/macdeployqt]

The script expects a built macOS bundle:
  <build-dir>/MaiChat.app

Outputs:
  <out-dir>/MaiChat-macos-arm64/
  <out-dir>/MaiChat-macos-arm64-<date>-<git-short-hash>.dmg
USAGE
}

build_dir="build"
out_dir="dist"
macdeployqt=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      build_dir="${2:-}"
      shift
      ;;
    --out-dir)
      out_dir="${2:-}"
      shift
      ;;
    --macdeployqt)
      macdeployqt="${2:-}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(git -C "$project_root" rev-parse --show-toplevel)"
build_path="$project_root/$build_dir"
app_name="MaiChat.app"
source_app="$build_path/$app_name"

if [[ ! -d "$source_app" ]]; then
  cat >&2 <<EOF
Missing app bundle: $source_app

Build it first, for example:
  cmake -S MaiChat/desktop -B MaiChat/desktop/$build_dir -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@5 -DCMAKE_BUILD_TYPE=Release
  cmake --build MaiChat/desktop/$build_dir --target maichat --config Release
EOF
  exit 1
fi

if [[ -z "$macdeployqt" ]]; then
  cache_path="$build_path/CMakeCache.txt"
  if [[ -f "$cache_path" ]]; then
    qt5_dir="$(sed -n 's/^Qt5_DIR:PATH=//p' "$cache_path" | head -1)"
    if [[ -n "$qt5_dir" ]]; then
      candidate="$(cd "$qt5_dir/../../.." && pwd)/bin/macdeployqt"
      if [[ -x "$candidate" ]]; then
        macdeployqt="$candidate"
      fi
    fi
  fi
fi

if [[ -z "$macdeployqt" ]]; then
  macdeployqt="$(command -v macdeployqt || true)"
fi

if [[ -z "$macdeployqt" || ! -x "$macdeployqt" ]]; then
  echo "Unable to locate macdeployqt. Pass --macdeployqt /path/to/macdeployqt." >&2
  exit 1
fi

dist_root="$project_root/$out_dir"
staging="$dist_root/MaiChat-macos-arm64"
staged_app="$staging/$app_name"
rm -rf "$staging"
mkdir -p "$staging"

ditto "$source_app" "$staged_app"

"$macdeployqt" "$staged_app" -always-overwrite

sdk_source="$project_root/vendor/tencent-im/macos/ImSDKForMac_Plus.framework"
if [[ ! -d "$sdk_source" ]]; then
  echo "Missing native IM SDK framework: $sdk_source" >&2
  exit 1
fi

sdk_target_dir="$staged_app/Contents/MacOS/vendor/tencent-im/macos"
mkdir -p "$sdk_target_dir"
ditto "$sdk_source" "$sdk_target_dir/ImSDKForMac_Plus.framework"

cat > "$staging/使用说明.txt" <<'README'
MaiChat 桌面客户端（macOS）
===================================

安装：将 MaiChat.app 拖到右侧 Applications 文件夹。
运行：复制完成后，从“应用程序”或 Launchpad 打开 MaiChat。

- 无需在目标机器安装 Qt，Qt 运行时已随 .app 旁挂。
- 原生 IM SDK 已随包放在 .app 内部，请不要手动移动 .app 内的 vendor 目录。
- 首次启动在登录页输入账号 ID 后回车即可登录。
- 未签名应用首次打开如被 macOS 拦截，可在 Finder 中右键应用并选择“打开”。
README

# Standard drag-to-install layout: users can drag the app onto Applications.
ln -s /Applications "$staging/Applications"

# Ad-hoc signing keeps embedded frameworks loadable without requiring a Developer ID.
if command -v codesign >/dev/null 2>&1; then
  codesign --force --deep --sign - "$staged_app"
  codesign --verify --deep --strict "$staged_app"
fi

bundle_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$staged_app/Contents/Info.plist" 2>/dev/null || true)"
short_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$staged_app/Contents/Info.plist" 2>/dev/null || true)"
if [[ -z "$bundle_version" || -z "$short_version" ]]; then
  echo "Invalid app bundle: CFBundleVersion and CFBundleShortVersionString must be set." >&2
  exit 1
fi

git_hash="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || true)"
if [[ -z "$git_hash" ]]; then
  git_hash="unknown"
fi

dmg_name="MaiChat-macos-arm64-$(date +%Y%m%d)-$git_hash.dmg"
dmg_path="$dist_root/$dmg_name"
rw_dmg="$dist_root/.${dmg_name%.dmg}-rw.dmg"
mount_dir=""
mounted_device=""
dmgbuild_script="$repo_root/node_modules/dmg-builder/vendor/dmgbuild/core.py"

cleanup() {
  if [[ -n "$mounted_device" ]]; then
    hdiutil detach "$mounted_device" -quiet || true
  fi
  rm -f "$rw_dmg"
}
trap cleanup EXIT

rm -f "$dmg_path" "$rw_dmg"
hdiutil create \
  -volname "MaiChat" \
  -srcfolder "$staging" \
  -ov \
  -format UDRW \
  -fs HFS+ \
  "$rw_dmg"

attach_output="$(hdiutil attach "$rw_dmg" -readwrite -noverify -noautoopen)"
mounted_device="$(printf '%s\n' "$attach_output" | awk '/Apple_HFS/ {print $1; exit}')"
mount_dir="$(printf '%s\n' "$attach_output" | sed -nE 's|^/dev/[^[:space:]]+[[:space:]]+Apple_HFS[[:space:]]+||p' | head -1)"
if [[ -z "$mounted_device" || -z "$mount_dir" || ! -d "$mount_dir" ]]; then
  echo "Unable to mount writable DMG." >&2
  exit 1
fi

if [[ ! -f "$dmgbuild_script" ]]; then
  echo "Missing DMG layout helper: $dmgbuild_script" >&2
  echo "Install the repository's Node dependencies before packaging." >&2
  exit 1
fi

# Write Finder metadata directly so the install layout does not depend on the
# packaging machine allowing Finder to persist .DS_Store files.
env \
  volumePath="$mount_dir" \
  iconSize=96 \
  iconTextSize=13 \
  windowX=120 \
  windowY=120 \
  windowWidth=580 \
  windowHeight=380 \
  backgroundColor="#ffffff" \
  iconLocations="'MaiChat.app': (150, 165), 'Applications': (430, 165), '使用说明.txt': (290, 315)" \
  python3 "$dmgbuild_script"

sync
if [[ ! -f "$mount_dir/.DS_Store" ]]; then
  echo "Unable to create the DMG Finder layout." >&2
  exit 1
fi
hdiutil detach "$mounted_device" -quiet
mounted_device=""
hdiutil convert "$rw_dmg" -format UDZO -imagekey zlib-level=9 -ov -o "$dmg_path"
rm -f "$rw_dmg"

size="$(du -h "$dmg_path" | awk '{print $1}')"
echo
echo "Qt IM macOS DMG:"
echo "  Directory: $staging"
echo "  Image:     $dmg_path ($size)"
echo "  Version:   $short_version ($bundle_version)"
