#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Package the Electron desktop app for macOS.

Usage:
  scripts/package-macos-electron.sh [--skip-aicli] [--skip-build]

Outputs:
  release/MultiAICode-<version>-arm64.dmg
  release/MultiAICode-<version>-arm64.dmg.blockmap

Options:
  --skip-aicli   Do not rebuild bundled codex/opencode before packaging.
  --skip-build   Do not run electron-builder; only print existing artifacts.
  -h, --help     Show this help.
USAGE
}

skip_aicli=0
skip_build=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-aicli)
      skip_aicli=1
      ;;
    --skip-build)
      skip_build=1
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

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if [[ "$skip_aicli" -eq 0 ]]; then
  npm run build:aicli
fi

if [[ "$skip_build" -eq 0 ]]; then
  npm run dist:mac
fi

artifacts_file="$(mktemp)"
find release -maxdepth 1 -type f \( -name 'MultiAICode-*-arm64.dmg' -o -name 'MultiAICode-*-arm64.dmg.blockmap' \) | sort > "$artifacts_file"
if [[ ! -s "$artifacts_file" ]]; then
  rm -f "$artifacts_file"
  echo "No macOS Electron artifacts found under release/." >&2
  exit 1
fi

echo
echo "Electron macOS artifacts:"
while IFS= read -r artifact; do
  size="$(du -h "$artifact" | awk '{print $1}')"
  echo "  $artifact ($size)"
done < "$artifacts_file"
rm -f "$artifacts_file"
