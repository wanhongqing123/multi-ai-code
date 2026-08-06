#!/usr/bin/env python3
"""Resolve verified rusty_v8 artifacts for the bundled Codex build."""

from __future__ import annotations

import json
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
CODEX_SCRIPTS = REPO_ROOT / "third_party" / "aicli" / "codex" / "scripts"
sys.path.insert(0, str(CODEX_SCRIPTS))

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
