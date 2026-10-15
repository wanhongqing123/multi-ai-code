#!/usr/bin/env python3
"""文件作用：合并真实 Xcode 与移动 Agent 构建命令，供 clangd 索引 iOS C++ 文件。

脚本保留 SDK、架构、宏和头文件路径，以工程实际编译命令为准，不凭猜测拼接参数。
"""

import argparse
import json
from pathlib import Path
import subprocess


def find_cpp_compilations(build_log: Path) -> list[dict[str, str]]:
    """匹配 Xcode 的 CompileC 记录和紧随其后的 clang 命令。"""
    lines = build_log.read_text(errors="replace").splitlines()
    entries = []
    for index, line in enumerate(lines):
        if not line.startswith("CompileC ") or ".cpp normal " not in line:
            continue
        parts = line.split()
        source = next((part for part in parts if part.endswith(".cpp")), None)
        if source is None:
            continue
        command = next(
            (candidate.strip() for candidate in lines[index + 1:index + 12]
             if candidate.lstrip().startswith("/Applications/Xcode.app/")
             and " -c " in candidate),
            None,
        )
        if command is None:
            continue
        entries.append({"directory": str(build_log.parent), "file": source,
                        "command": command})
    return entries


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xcode_build_log", type=Path,
                        help="包含 CompileC 命令的 xcodebuild build 日志")
    arguments = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    destination = project / "compile_commands.json"
    entries = find_cpp_compilations(arguments.xcode_build_log)
    if destination.exists():
        # 增量构建可能没有重新编译桥接文件，沿用先前真实 Xcode 命令。
        existing = json.loads(destination.read_text())
        present = {entry["file"] for entry in entries}
        entries.extend(entry for entry in existing if entry["file"] not in present)
    adapter_database = project.parents[1] / "MobileAgentAdapter" / "compile_commands.json"
    if adapter_database.exists():
        # 相册工具由 CMake 而非 Xcode 编译，使用其 iOS 平台的真实选项。
        source_root = project / "MaiChat"
        present = {entry["file"] for entry in entries}
        for entry in json.loads(adapter_database.read_text()):
            source = Path(entry["file"])
            if (source.is_relative_to(source_root) and source.suffix == ".cpp" and
                    entry["file"] not in present):
                entries.append(entry)
                present.add(entry["file"])
    if not entries:
        parser.error("没有 iOS C++ 编译命令；请先构建 App 和移动 Agent 适配层")
    for entry in entries:
        if entry["file"].endswith("MaiFfplayIosBridge.cpp"):
            entry["directory"] = str(project)
    destination.write_text(json.dumps(entries, ensure_ascii=False, indent=2) + "\n")
    print(f"{destination}: {len(entries)} C++ 编译命令")
    subprocess.run(["python3", str(project.parents[1] / "scripts/generate-compile-commands.py"),
                    "--platform", "ios"], check=True)


if __name__ == "__main__":
    main()
