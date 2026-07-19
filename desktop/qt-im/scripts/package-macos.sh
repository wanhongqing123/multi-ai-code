#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Package the Qt Desktop IM app for macOS.

Usage:
  desktop/qt-im/scripts/package-macos.sh [--build-dir build] [--out-dir dist] [--macdeployqt /path/to/macdeployqt]

The script expects a built macOS bundle:
  <build-dir>/Multi-AI Code IM.app

Outputs:
  <out-dir>/MultiAIIM-macos-arm64/
  <out-dir>/MultiAIIM-macos-arm64-<date>-<git-short-hash>.dmg
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
app_name="Multi-AI Code IM.app"
source_app="$build_path/$app_name"

if [[ ! -d "$source_app" ]]; then
  cat >&2 <<EOF
Missing app bundle: $source_app

Build it first, for example:
  cmake -S desktop/qt-im -B desktop/qt-im/$build_dir -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@5 -DCMAKE_BUILD_TYPE=Release
  cmake --build desktop/qt-im/$build_dir --target multi_ai_im_desktop --config Release
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
staging="$dist_root/MultiAIIM-macos-arm64"
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
Multi-AI Code IM 桌面客户端（macOS）
===================================

运行：双击 Multi-AI Code IM.app。

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

git_hash="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || true)"
if [[ -z "$git_hash" ]]; then
  git_hash="unknown"
fi

dmg_name="MultiAIIM-macos-arm64-$(date +%Y%m%d)-$git_hash.dmg"
dmg_path="$dist_root/$dmg_name"
rm -f "$dmg_path"
hdiutil create \
  -volname "Multi-AI Code IM" \
  -srcfolder "$staging" \
  -ov \
  -format UDZO \
  -fs APFS \
  "$dmg_path"

size="$(du -h "$dmg_path" | awk '{print $1}')"
echo
echo "Qt IM macOS DMG:"
echo "  Directory: $staging"
echo "  Image:     $dmg_path ($size)"
