#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Upload macOS release artifacts to GitHub Releases.

Usage:
  scripts/upload-macos-releases.sh [--date YYYYMMDD] [--dry-run]

Environment:
  ELECTRON_RELEASE_TAG  Override Electron release tag. Defaults to electron-<date>.
  QT_IM_RELEASE_TAG     Override Qt IM release tag. Defaults to qt-im-<date>.

Options:
  --date YYYYMMDD       Release date suffix. Defaults to today.
  --dry-run             Print upload commands without executing them.
  -h, --help            Show this help.
USAGE
}

release_date="$(date +%Y%m%d)"
dry_run=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --date)
      release_date="${2:-}"
      shift
      ;;
    --dry-run)
      dry_run=1
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

if [[ ! "$release_date" =~ ^[0-9]{8}$ ]]; then
  echo "--date must use YYYYMMDD format." >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

electron_tag="${ELECTRON_RELEASE_TAG:-electron-$release_date}"
qt_im_tag="${QT_IM_RELEASE_TAG:-qt-im-$release_date}"

latest_file() {
  local pattern="$1"
  local file_list
  file_list="$(mktemp)"
  find . -path "$pattern" -type f -print0 > "$file_list"
  if [[ ! -s "$file_list" ]]; then
    rm -f "$file_list"
    return
  fi
  xargs -0 ls -t < "$file_list" | head -1 | sed 's#^\./##'
  rm -f "$file_list"
}

electron_dmg="$(latest_file './release/MultiAICode-*-arm64.dmg')"
electron_blockmap="$(latest_file './release/MultiAICode-*-arm64.dmg.blockmap')"
qt_im_zip="$(latest_file "./desktop/qt-im/dist/MultiAIIM-macos-arm64-$release_date-*.zip")"

if [[ -z "$electron_dmg" || -z "$electron_blockmap" ]]; then
  echo "Missing Electron macOS artifacts under release/." >&2
  exit 1
fi

if [[ -z "$qt_im_zip" ]]; then
  echo "Missing Qt IM macOS artifact for $release_date under desktop/qt-im/dist/." >&2
  exit 1
fi

if [[ "$dry_run" -eq 1 ]]; then
  echo "Dry run:"
  echo "  gh release upload $electron_tag $electron_dmg $electron_blockmap --clobber"
  echo "  gh release upload $qt_im_tag $qt_im_zip --clobber"
  exit 0
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "GitHub CLI is required. Install it with: brew install gh" >&2
  exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
  echo "GitHub CLI is not authenticated. Run: gh auth login" >&2
  exit 1
fi

gh release upload "$electron_tag" "$electron_dmg" "$electron_blockmap" --clobber
gh release upload "$qt_im_tag" "$qt_im_zip" --clobber

echo
echo "Uploaded macOS release artifacts:"
echo "  $electron_tag: $electron_dmg, $electron_blockmap"
echo "  $qt_im_tag: $qt_im_zip"
