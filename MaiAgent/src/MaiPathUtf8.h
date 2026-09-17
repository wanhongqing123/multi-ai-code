#pragma once

#include <filesystem>
#include <string>

// UTF-8 字符串 <-> std::filesystem::path 的转换。
//
// **在 Windows 上必须走这里，不能用 fs::path(s) 和 p.string()。**
//
// 原因：MSVC 的 std::filesystem 把 narrow 字符串按**当前 ANSI 代码页**
// 解释（中文机器是 GBK），而我们的路径全部来自 JSON，是 UTF-8。
// 直接 fs::path(utf8String) 会把 UTF-8 字节当 GBK 解，
// 中文文件名立刻变乱码——实测直接让进程挂掉（STATUS_STACK_BUFFER_OVERRUN），
// 不是优雅失败。
//
// u8path / u8string 明确指定了 UTF-8，跨平台行为一致。
// 它们在 C++20 被弃用，但本项目是 C++17，这是标准给的正解。
class MaiPathUtf8 {
public:
    static std::filesystem::path fromUtf8(const std::string& text);
    static std::string toUtf8(const std::filesystem::path& path);
    // generic 形式统一用 / 分隔，这样模型看到的路径在三个平台上长得一样。
    static std::string toUtf8Generic(const std::filesystem::path& path);
};
