#!/usr/bin/env python3
"""文件作用：给 MaiChat 根目录的共用媒体与宿主工具源码生成 clangd 编译数据库。

只汇集已有桌面、Android 和 iOS 构建实际生成的命令；平台专有源码缺少该平台构建时保持缺失，
不伪造宏或 SDK 路径。clangd 从 Media、MaiChatTools 向上查找时会自动读到根目录数据库。
"""

import argparse
import json
import os
import tempfile
from pathlib import Path


MAICHAT_ROOT = Path(__file__).resolve().parents[1]
COMPILATION_DATABASES = {
    "desktop": MAICHAT_ROOT / "desktop/compile_commands.json",
    "android": MAICHAT_ROOT / "android/MaiChat/app/compile_commands.json",
    "ios": MAICHAT_ROOT / "MobileAgentAdapter/compile_commands.json",
}
PLATFORM_PRIORITY = {
    "desktop": ("desktop", "ios", "android"),
    "ios": ("ios", "desktop", "android"),
    "android": ("android", "ios", "desktop"),
}
SHARED_SOURCE_ROOTS = (MAICHAT_ROOT / "Media", MAICHAT_ROOT / "MaiChatTools")


def source_path(entry: dict[str, object]) -> Path:
    """按编译命令自己的工作目录解析相对路径，避免误用脚本的当前目录。"""
    source = Path(str(entry["file"]))
    if not source.is_absolute():
        source = Path(str(entry["directory"])) / source
    return source.resolve()


def collect_commands(preferred_platform: str) -> list[dict[str, object]]:
    """优先选目标平台的真实命令，其余平台只补目标平台没有编译的文件。"""
    commands: dict[Path, dict[str, object]] = {}
    for platform in PLATFORM_PRIORITY[preferred_platform]:
        database = COMPILATION_DATABASES[platform]
        if not database.is_file():
            continue
        for entry in json.loads(database.read_text(encoding="utf-8")):
            source = source_path(entry)
            if not any(source.is_relative_to(root) for root in SHARED_SOURCE_ROOTS):
                continue
            if source not in commands:
                commands[source] = {**entry, "file": str(source)}
    return list(commands.values())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=COMPILATION_DATABASES,
                        default="desktop", help="共用源码优先使用哪个平台的实际编译选项")
    arguments = parser.parse_args()
    commands = collect_commands(arguments.platform)
    if not commands:
        parser.error("找不到共用源码的实际编译命令；请先构建相应平台")
    destination = MAICHAT_ROOT / "compile_commands.json"
    # clangd 可能正读取旧文件；先完整写入临时文件，再一次性替换。
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=MAICHAT_ROOT,
                                         prefix=".compile_commands.", suffix=".tmp",
                                         delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(commands, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f"{destination}: {len(commands)} 个真实 C/C++ 编译命令，优先平台 {arguments.platform}")


if __name__ == "__main__":
    main()
