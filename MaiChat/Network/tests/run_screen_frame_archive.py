#!/usr/bin/env python3
"""Local archive acceptance with an independent process reopening persisted data."""
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="maichat-screen-archive-") as directory:
    workspace = pathlib.Path(directory) / "Unicode-\u4f1a\u8bdd space"
    workspace.mkdir()
    subprocess.run([sys.argv[1], "--run", str(workspace)], check=True, timeout=60)
    races = [subprocess.Popen([sys.argv[1], "--process-race", str(workspace)],
                              stdout=subprocess.PIPE, text=True) for _ in range(2)]
    results = [process.communicate(timeout=20)[0].strip() for process in races]
    assert all(process.returncode == 0 for process in races)
    assert sorted(results) == ["race.accepted", "race.rate_limited"], results
    subprocess.run([sys.argv[1], "--reopen", str(workspace)], check=True, timeout=60)
    stale_workspace = pathlib.Path(directory) / "moved-workspace"
    stale_workspace.mkdir()
    subprocess.run([sys.argv[1], "--stale-batch", str(stale_workspace)], check=True, timeout=60)
