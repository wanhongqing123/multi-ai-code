// 文件作用：定义 Agent 会话及其持久化状态。
// 主要实现：isRoot、isUntitled；对应公开接口中的状态和错误处理。
#include "MaiSession.h"

#include <cstddef>

MaiError maiValidateSessionTitle(const std::string& title) {
    if (title.empty() || title.size() > 480)
        return {
            MaiErrorCode::InvalidInput,
            "session title must contain text and be at most 120 characters and 480 UTF-8 bytes"};
    std::size_t position = 0;
    std::size_t characters = 0;
    bool hasText = false;
    while (position < title.size()) {
        const auto first = static_cast<unsigned char>(title[position]);
        std::size_t width = 1;
        unsigned scalar = first;
        // 沿用现有 MaiFileTools::isValidUtf8 的严格边界检查；这里只做标题的有限校验，
        // 不引入另一套通用字符库，更不让会话元数据依赖 PDF 引擎。
        if (first >= 0x80) {
            if (first >= 0xc2 && first <= 0xdf)
                width = 2;
            else if (first >= 0xe0 && first <= 0xef)
                width = 3;
            else if (first >= 0xf0 && first <= 0xf4)
                width = 4;
            else
                return {MaiErrorCode::InvalidInput, "session title must contain valid UTF-8 text"};
            if (position + width > title.size())
                return {MaiErrorCode::InvalidInput, "session title must contain valid UTF-8 text"};
            const auto second = static_cast<unsigned char>(title[position + 1]);
            if (second < 0x80 || second > 0xbf || (first == 0xe0 && second < 0xa0) ||
                (first == 0xed && second > 0x9f) || (first == 0xf0 && second < 0x90) ||
                (first == 0xf4 && second > 0x8f))
                return {MaiErrorCode::InvalidInput, "session title must contain valid UTF-8 text"};
            scalar = first & ((1u << (7 - width)) - 1);
            for (std::size_t offset = 1; offset < width; ++offset) {
                const auto next = static_cast<unsigned char>(title[position + offset]);
                if (next < 0x80 || next > 0xbf)
                    return {MaiErrorCode::InvalidInput,
                            "session title must contain valid UTF-8 text"};
                scalar = (scalar << 6) | (next & 0x3f);
            }
        }
        position += width;
        if (++characters > 120 || scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f) ||
            scalar == 0x2028 || scalar == 0x2029)
            return {MaiErrorCode::InvalidInput,
                    "session title must be a single plain-text line of at most 120 characters"};
        // Unicode White_Space 中的控制换行已在上面拒绝；其余空白不能单独充当名称。
        const bool whitespace = scalar == 0x20 || scalar == 0xa0 || scalar == 0x1680 ||
                                (scalar >= 0x2000 && scalar <= 0x200a) || scalar == 0x202f ||
                                scalar == 0x205f || scalar == 0x3000;
        hasText = hasText || !whitespace;
    }
    if (!hasText) return {MaiErrorCode::InvalidInput, "session title must not be blank"};
    return {};
}

bool MaiSession::isRoot() const {
    return parentId.empty();
}

bool MaiSession::isUntitled() const {
    // 显式名称优先于占位字符串判断，保证合法的同名标题跨轮及重启后仍由用户控制。
    return !titleEdited && (title.empty() || title == kMaiDefaultSessionTitle);
}
