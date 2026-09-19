#include "MaiFilePath.h"

// 纯词法的那一半：不碰磁盘，不调系统调用，在任何平台上给同样的输入得同样的结果。
// fromUtf8 / toUtf8 在各平台的文件里（那两个要调系统 API）。

namespace {

#if defined(_WIN32)
constexpr MaiFilePath::CharType kPreferredSeparator = L'\\';
constexpr const MaiFilePath::CharType* kSeparators = L"\\/";
#else
constexpr MaiFilePath::CharType kPreferredSeparator = '/';
constexpr const MaiFilePath::CharType* kSeparators = "/";
#endif

// 盘符 / UNC 前缀的长度。没有就返回 0。
//
// 这一段要单独拎出来，否则逐段比对时 "C:" 会被当成一段普通目录，
// 而 "C:" 和 "c:" 在 Windows 上指的是同一个盘。
std::size_t rootPrefixLength(const MaiFilePath::StringType& value) {
#if defined(_WIN32)
    // \\server\share 或 \\?\C:\...
    if (value.size() >= 2 && MaiFilePath::isSeparator(value[0]) &&
        MaiFilePath::isSeparator(value[1])) {
        return 2;
    }
    // C: / C:\ .
    if (value.size() >= 2 && value[1] == L':') return 2;
#endif
    (void)value;
    return 0;
}

}  // namespace

bool MaiFilePath::isSeparator(CharType character) {
    for (const CharType* it = kSeparators; *it != 0; ++it) {
        if (*it == character) return true;
    }
    return false;
}

bool MaiFilePath::isAbsolute() const {
    if (mValue.empty()) return false;
    const std::size_t prefix = rootPrefixLength(mValue);
#if defined(_WIN32)
    // "C:" 单独出现是**相对**路径（当前盘的当前目录），"C:\" 才是绝对。
    // 这个区别在 Windows 上很容易漏，漏了会让 "C:x" 被当成绝对路径。
    if (prefix == 2 && mValue[1] == L':')
        return mValue.size() > 2 && isSeparator(mValue[2]);
    if (prefix == 2) return true;  // \\server\share
#endif
    (void)prefix;
    return isSeparator(mValue[0]);
}

MaiFilePath MaiFilePath::append(const MaiFilePath& tail) const {
    if (tail.isEmpty()) return *this;
    if (mValue.empty() || tail.isAbsolute()) return tail;

    StringType joined = mValue;
    if (!isSeparator(joined.back())) joined += kPreferredSeparator;

    // 去掉 tail 开头多余的分隔符，免得拼出 "a//b"
    std::size_t start = 0;
    while (start < tail.mValue.size() && isSeparator(tail.mValue[start])) ++start;
    joined.append(tail.mValue, start, StringType::npos);
    return MaiFilePath(std::move(joined));
}

MaiFilePath MaiFilePath::dirName() const {
    const std::size_t prefix = rootPrefixLength(mValue);

    // 根后面紧跟的那个分隔符也属于"根"，砍到这儿就不能再往里砍了。少了这一步，
    // "/" 的父目录会算成空串——而 createDirectories 是靠 dirName 往上递归的，空串会让它以为到顶了，
    // 实际是把根给丢了。
    std::size_t rootEnd = prefix;
    if (rootEnd < mValue.size() && isSeparator(mValue[rootEnd])) ++rootEnd;

    std::size_t end = mValue.size();
    // 先吃掉结尾的分隔符（"a/b/" 的父目录是 "a"）
    while (end > rootEnd && isSeparator(mValue[end - 1])) --end;
    // 再退掉最后一段
    while (end > rootEnd && !isSeparator(mValue[end - 1])) --end;
    // 把那一段前面的分隔符也吃掉
    while (end > rootEnd && isSeparator(mValue[end - 1])) --end;

    // "a" 这种只有一段的相对路径，没有父目录
    if (end == 0) return MaiFilePath();
    return MaiFilePath(mValue.substr(0, end < rootEnd ? rootEnd : end));
}

MaiFilePath MaiFilePath::baseName() const {
    const std::size_t prefix = rootPrefixLength(mValue);

    std::size_t end = mValue.size();
    while (end > prefix && isSeparator(mValue[end - 1])) --end;

    std::size_t start = end;
    while (start > prefix && !isSeparator(mValue[start - 1])) --start;

    return MaiFilePath(mValue.substr(start, end - start));
}

std::vector<MaiFilePath::StringType> MaiFilePath::components() const {
    std::vector<StringType> out;
    if (mValue.empty()) return out;

    const std::size_t prefix = rootPrefixLength(mValue);
    std::size_t index = 0;

    // 根单独成一段（"C:" 或者第一个 "/"），这样绝对路径和相对路径不会被切成看起来一样的东西。
    if (prefix > 0) {
        out.push_back(mValue.substr(0, prefix));
        index = prefix;
        if (index < mValue.size() && isSeparator(mValue[index])) {
            out.push_back(StringType(1, kPreferredSeparator));
            while (index < mValue.size() && isSeparator(mValue[index])) ++index;
        }
    } else if (isSeparator(mValue[0])) {
        out.push_back(StringType(1, kPreferredSeparator));
        while (index < mValue.size() && isSeparator(mValue[index])) ++index;
    }

    while (index < mValue.size()) {
        const std::size_t start = index;
        while (index < mValue.size() && !isSeparator(mValue[index])) ++index;
        if (index > start) out.push_back(mValue.substr(start, index - start));
        while (index < mValue.size() && isSeparator(mValue[index])) ++index;
    }
    return out;
}

bool MaiFilePath::isParentOf(const MaiFilePath& child) const {
    if (mValue.empty() || child.isEmpty()) return false;

    const std::vector<StringType> mine = components();
    const std::vector<StringType> theirs = child.components();

    // 一个路径不是它自己的父目录（和 chromium 的 IsParent 一致）
    if (theirs.size() <= mine.size()) return false;

    for (std::size_t i = 0; i < mine.size(); ++i) {
#if defined(_WIN32)
        // Windows 的路径比较不分大小写。不这么做的话，
        // 模型送来"C:\Work\a.txt" 而 root 是 "C:\work"，会被误判成越界。
        if (mine[i].size() != theirs[i].size()) return false;
        for (std::size_t n = 0; n < mine[i].size(); ++n) {
            wchar_t left = mine[i][n];
            wchar_t right = theirs[i][n];
            if (left >= L'A' && left <= L'Z') left = static_cast<wchar_t>(left - L'A' + L'a');
            if (right >= L'A' && right <= L'Z') right = static_cast<wchar_t>(right - L'A' + L'a');
            if (left != right) return false;
        }
#else
        if (mine[i] != theirs[i]) return false;
#endif
    }
    return true;
}
