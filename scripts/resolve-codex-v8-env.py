#!/usr/bin/env python3
"""Resolve verified rusty_v8 artifacts for the bundled Codex build."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
CODEX_ROOT = REPO_ROOT / "third_party" / "aicli" / "codex"
CODEX_SCRIPTS = CODEX_ROOT / "scripts"
sys.path.insert(0, str(CODEX_SCRIPTS))

# 上游（2026-08 起）要求 codex_package.targets 在导入时就能读到 CODEX_REPO_ROOT，
# 否则直接 raise。上游自己是靠 `just assemble-codex-package` 设的，我们不走那条路，
# 所以在这里显式指向子模块根目录。
os.environ.setdefault("CODEX_REPO_ROOT", str(CODEX_ROOT))

from codex_package.targets import TARGET_SPECS  # noqa: E402
from codex_package.v8 import resolve_codex_v8_cargo_env  # noqa: E402


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in TARGET_SPECS:
        supported = ", ".join(sorted(TARGET_SPECS))
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} <target>; supported: {supported}")

    env = resolve_codex_v8_cargo_env(TARGET_SPECS[sys.argv[1]])
    print(json.dumps(env))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
