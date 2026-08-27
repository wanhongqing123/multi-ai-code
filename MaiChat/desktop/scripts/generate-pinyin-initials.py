#!/usr/bin/env python3
"""生成汉字拼音首字母表（src/model/PinyinInitials.cpp）。

数据来源：pypinyin 的 pinyin_dict.json（MIT 许可；其字音数据源自 Unicode 的
Unihan 数据库）。本脚本不安装 pypinyin——wheel 就是个 zip，直接读出所需的那一个
文件即可，因此生成过程不引入运行时依赖，构建时也不需要联网。

用法：
    python generate-pinyin-initials.py <pypinyin-*.whl>

取回 wheel（只需在更新表时做一次）：
    python -m pip download pypinyin --no-deps -d <目录>

为什么只存首字母、不存全拼：
    搜索侧只做「首字母连续匹配」。实测（用户 7340 条真实消息）显示，
    首字母做子序列匹配即使加上跨度限制仍会命中全部消息的 16~29%，不可用；
    改为要求连续之后，4 字母查询降到 0.2~0.7%。全拼匹配需要音节切分，
    是另一个量级的工作，等首字母版跑一阵子再决定要不要做。

为什么用位掩码：
    每个字一个 26 位掩码，第 i 位表示「该字某个读音的首字母是 'a'+i」。
    多音字天然就是多个位，匹配时按位测试即可，不需要任何特殊分支。
    表大小 20992 × 4 字节 ≈ 82 KB，编进二进制，查表 O(1)。
"""

import json
import sys
import unicodedata
import zipfile
from pathlib import Path

# 汉字常用区（CJK Unified Ideographs）。扩展区 B 及以后在聊天里几乎不出现，
# 纳入会让表膨胀数倍却换不来实际命中，因此不收。
LOW = 0x4E00
HIGH = 0x9FFF


def strip_tone(syllable: str) -> str:
    """去掉声调符号，并把 ü 归一成 v。只取首字母时其实只关心第一个字符，
    但拼音里 'ǖ' 这类带调元音会出现在首位（如 '虚' 的某些注音风格），
    所以统一先规范化再取首字符。"""
    out = []
    for ch in unicodedata.normalize("NFD", syllable):
        if unicodedata.combining(ch):
            continue
        out.append("v" if ch in "üÜ" else ch)
    return "".join(out).lower()


def build_masks(pinyin_dict: dict) -> list:
    masks = [0] * (HIGH - LOW + 1)
    for key, value in pinyin_dict.items():
        code_point = int(key)
        if not (LOW <= code_point <= HIGH):
            continue
        mask = 0
        for syllable in value.split(","):
            initial = strip_tone(syllable.strip())[:1]
            if "a" <= initial <= "z":
                mask |= 1 << (ord(initial) - ord("a"))
        masks[code_point - LOW] = mask
    return masks


def render(masks: list) -> str:
    covered = sum(1 for m in masks if m)
    polyphonic = sum(1 for m in masks if bin(m).count("1") > 1)
    lines = [
        "// 本文件由 scripts/generate-pinyin-initials.py 生成，请勿手工编辑。",
        "//",
        "// 数据来源：pypinyin 的 pinyin_dict.json（MIT），其字音数据源自 Unicode 的",
        "// Unihan 数据库（Unicode License）。要更新此表，重新运行该脚本即可。",
        "//",
        f"// 覆盖 U+{LOW:04X}–U+{HIGH:04X} 共 {len(masks)} 个码点，其中 {covered} 个有拼音"
        f"（{covered / len(masks) * 100:.1f}%），{polyphonic} 个是首字母不止一个的多音字。",
        "",
        '#include "model/PinyinInitials.h"',
        "",
        "namespace PinyinInitials {",
        "",
        "// 每项是一个 26 位掩码：第 i 位为 1 表示该字存在首字母为 'a'+i 的读音。",
        "const quint32 kInitialMask[kTableSize] = {",
    ]
    row = []
    for index, mask in enumerate(masks):
        row.append(str(mask))
        if len(row) == 16:
            lines.append("    " + ",".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ",".join(row) + ",")
    lines += ["};", "", "}  // namespace PinyinInitials", ""]
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    wheel = Path(sys.argv[1])
    if not wheel.is_file():
        print(f"找不到 wheel：{wheel}")
        return 1
    with zipfile.ZipFile(wheel) as archive:
        raw = archive.read("pypinyin/pinyin_dict.json").decode("utf-8")
    masks = build_masks(json.loads(raw))
    target = Path(__file__).resolve().parent.parent / "src" / "model" / "PinyinInitials.cpp"
    target.write_text(render(masks), encoding="utf-8", newline="\n")
    covered = sum(1 for m in masks if m)
    print(f"已写入 {target}")
    print(f"  码点 {len(masks)}，有拼音 {covered}（{covered / len(masks) * 100:.1f}%）")
    print(f"  源码 {target.stat().st_size / 1024:.0f} KB，二进制约 {len(masks) * 4 / 1024:.0f} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
