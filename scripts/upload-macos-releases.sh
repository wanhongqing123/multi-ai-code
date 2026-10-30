#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Upload Multi-AI Code macOS release artifacts to GitHub Releases.

Usage:
  scripts/upload-macos-releases.sh [--date YYYYMMDD] [--dry-run]

Environment:
  ELECTRON_RELEASE_TAG  Override Electron release tag. Defaults to electron-<date>.

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

# 产品已拆仓；本仓库发布只检查 Multi-AI Code 的安装包和更新元数据。
electron_dmg="$(latest_file './release/MultiAICode-*-arm64.dmg')"
electron_blockmap="$(latest_file './release/MultiAICode-*-arm64.dmg.blockmap')"

if [[ -z "$electron_dmg" || -z "$electron_blockmap" ]]; then
  echo "Missing Electron macOS artifacts under release/." >&2
  exit 1
fi

if [[ "$dry_run" -eq 1 ]]; then
  echo "Dry run:"
  echo "  gh release upload $electron_tag $electron_dmg $electron_blockmap --clobber"
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

echo
echo "Uploaded macOS release artifacts:"
echo "  $electron_tag: $electron_dmg, $electron_blockmap"
