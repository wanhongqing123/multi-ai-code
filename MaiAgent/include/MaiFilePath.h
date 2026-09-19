#pragma once

#include <string>
#include <vector>

// 路径值类型。设计照着 chromium 的 base::FilePath
// （E:\OpenSource\chromium\src\base\files\file_path.h）。
//
// ── 为什么不用 std::filesystem::path ────────────────────────────
// MSVC 的 fs::path 把 narrow 字符串按**当前 ANSI 代码页**解释（中文机器
// 是 GBK），而我们的路径全部来自 JSON，是 UTF-8。这个坑踩过一次：中文文件名不是优雅失败，
// 是进程直接挂掉（STATUS_STACK_BUFFER_OVERRUN）。
//
// ── 为什么内部存"平台原生类型"而不是统一 UTF-8 ──────────────────
// 这是从 chromium 学来的，理由是**正确性**不是性能：
//
//   Windows 文件名是 UTF-16，可能含未配对代理项，转成 UTF-8 再转回来
//   不保证原样。
//   POSIX 文件名是任意字节序列，**根本不保证是合法 UTF-8**。Linux 上
//   一个用 Latin-1 命名的文件，强行当 UTF-8 处理就会丢掉或改写它的名字。
//
// 所以内部一律用平台原生串：Windows 上 wstring，其它平台上 string。
// 只在两个边界上转 UTF-8——模型送进来的 JSON 参数，和回给模型的文本。那两处转换是本来就避不开的，
// 因为模型只会说 UTF-8。
class MaiFilePath {
public:
#if defined(_WIN32)
    // Windows 的文件名是 UTF-16。走宽字符 API（CreateFileW 之类），
    // 窄字符那套（CreateFileA）会经过 ANSI 代码页，正是我们要躲开的东西。
    using StringType = std::wstring;
#else
    using StringType = std::string;
#endif
    using CharType = StringType::value_type;

    MaiFilePath() = default;
    explicit MaiFilePath(StringType value) : mValue(std::move(value)) {}

    // ── 边界转换 ────────────────────────────────────────────────
    // 实现在各平台的文件里：Windows 走 MultiByteToWideChar，POSIX 上字节就是字节，原样拿着。
    static MaiFilePath fromUtf8(const std::string& utf8);
    std::string toUtf8() const;

    // 分隔符统一成 '/' 的 UTF-8 形式。给模型看的路径走这个——三个平台上长得一样，
    // 它回给我们的我们也认。
    std::string toGenericUtf8() const;

    const StringType& value() const {
        return mValue;
    }
    bool isEmpty() const {
        return mValue.empty();
    }

    // Windows 上 '/' 和 '\\' 都算分隔符；POSIX 上只有 '/'。
    static bool isSeparator(CharType character);

    // 认三种形态：/unix、C:\windows、\\server\share。
    bool isAbsolute() const;

    // 拼接。tail 是绝对路径时直接返回 tail。
    MaiFilePath append(const MaiFilePath& tail) const;

    // 去掉最后一段。纯字符串操作，**不解析 ".."**，
    // 和 chromium 的 DirName 一样——"../a" 的结果是 ".."。
    MaiFilePath dirName() const;

    // 最后一段。
    MaiFilePath baseName() const;

    // 切成一段一段，**带上根**，这样绝对路径和相对路径不会被混淆：
    //   POSIX:   "/foo/bar"   -> [ "/", "foo", "bar" ]
    //   Windows: "C:\foo\bar" -> [ "C:", "\\", "foo", "bar" ]
    std::vector<StringType> components() const;

    // this 是不是 child 的祖先。**纯词法**：不解析符号链接，也不处理 ".."。
    //
    // 判断"在不在某个目录下"必须逐段比，不能比字符串前缀：
    // "/server/app-secrets" 是 "/server/app" 的前缀，却不在那个目录里。
    // 但光有这个不够——调用方必须先把两边都解析成真实路径，
    // 否则一个符号链接就绕过去了（见 maiResolvePathWithinRoot）。
    bool isParentOf(const MaiFilePath& child) const;

    bool operator==(const MaiFilePath& other) const {
        return mValue == other.mValue;
    }
    bool operator!=(const MaiFilePath& other) const {
        return mValue != other.mValue;
    }

private:
    StringType mValue;
};
