#pragma once

#include <QChar>
#include <QtGlobal>

// 汉字拼音首字母表。只服务搜索，不做通用拼音转换。
//
// 表由 scripts/generate-pinyin-initials.py 生成（数据源自 pypinyin / Unihan），
// 每个字一个 26 位掩码，多音字就是多个位——匹配时按位测试，不需要特殊分支。
namespace PinyinInitials {

// 覆盖区间为 CJK 常用区 U+4E00–U+9FFF。扩展区在聊天正文里几乎不出现，
// 收进来会让表膨胀数倍却换不到实际命中。
constexpr char32_t kLowCodePoint = 0x4E00;
constexpr char32_t kHighCodePoint = 0x9FFF;
constexpr int kTableSize = static_cast<int>(kHighCodePoint - kLowCodePoint + 1);

extern const quint32 kInitialMask[kTableSize];

// 该字的首字母集合；不是收录范围内的汉字返回 0。
// 返回 0 同时也表示「这个字不参与拼音匹配」，调用方据此切断连续段。
inline quint32 maskFor(QChar ch) {
    const char32_t code = static_cast<char32_t>(ch.unicode());
    if (code < kLowCodePoint || code > kHighCodePoint) return 0;
    return kInitialMask[code - kLowCodePoint];
}

// letter 必须是小写 a–z；其余一律返回 false，避免把标点或数字当成首字母。
inline bool maskHasLetter(quint32 mask, QChar letter) {
    const ushort code = letter.unicode();
    if (code < 'a' || code > 'z') return false;
    return (mask >> (code - 'a')) & 1u;
}

}  // namespace PinyinInitials
