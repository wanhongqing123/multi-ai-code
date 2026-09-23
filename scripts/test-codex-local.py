#!/usr/bin/env python3
"""Run Codex's just test with an isolated home and local mock-server routing.

Usage: python3 scripts/test-codex-local.py -p codex-tui --test-threads 4
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    repository = Path(__file__).resolve().parents[1]
    environment = os.environ.copy()
    # The desktop host injects these into its terminals. They must not redirect
    # test databases or make snapshots depend on the user's current palette.
    host_settings = {
        "CODEX_HOME",
        "CODEX_SQLITE_HOME",
        "CODEX_SESSION_ID",
        "CODEX_THREAD_ID",
        "CODEX_DEFAULT_TERMINAL_BG",
        "CODEX_DEFAULT_TERMINAL_FG",
        "CODEX_SUPPRESS_OPTIONAL_WINDOWS_SANDBOX_PROMPT",
        "CODEX_JEDITERM_CURSOR_REPAIR",
        "COLORTERM",
        "FORCE_COLOR",
        "NO_COLOR",
    }
    for key in list(environment):
        if key in host_settings or key.startswith(("OPENAI_", "MULTI_AI_CODE_")):
            environment.pop(key, None)
    # reqwest can inherit macOS system proxy settings even when no proxy env
    # exists. Only local fixture connections bypass proxies here.
    exceptions = environment.get("NO_PROXY", environment.get("no_proxy", ""))
    exceptions = ",".join(filter(None, (exceptions, "127.0.0.1,localhost,::1")))
    environment.update(TERM="dumb", NO_PROXY=exceptions, no_proxy=exceptions)
    environment.setdefault("CARGO_BUILD_JOBS", "4")
    environment.setdefault("RUST_MIN_STACK", "8388608")
    with tempfile.TemporaryDirectory(prefix="codex-local-tests-") as directory:
        environment["CODEX_HOME"] = directory
        result = subprocess.run(
            ["just", "test", *sys.argv[1:]],
            cwd=repository / "third_party/aicli/codex/codex-rs",
            env=environment,
            check=False,
        )
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
