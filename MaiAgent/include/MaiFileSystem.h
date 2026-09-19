#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"

// 文件系统操作。**一律走系统 API**，不用 std::filesystem，不用 fstream。
//
// Windows 走宽字符那套（CreateFileW / FindFirstFileExW / ...），
// POSIX 走 open / read / opendir / stat 那套。理由和 MaiFilePath 一样：
// 窄字符的 Win32 API 会经过 ANSI 代码页，中文路径直接出错。
//
// 另外两个好处：
//   - 错误信息是真的。std::filesystem 把 Win32 错误码塞进 std::error_code
//     之后，"拒绝访问"和"路径不存在"都变成一句含糊的话。这里直接拿
//     GetLastError() / errno。
//   - 嵌入式和移动端的工具链未必带 <filesystem>（它是 STL 里挺重的一块），
//     而这个项目就是奔着那些平台去的。
//
// 形状参考 chromium 的 base/files/file_util.h 和 file_enumerator.h，
// 只取这个项目真正用到的那一小撮。

// 一个目录项。
struct MaiFileEntry {
    MaiFilePath path;      // 完整路径
    std::string nameUtf8;  // 只有最后一段，UTF-8
    bool isDirectory = false;
    std::uint64_t size = 0;  // 目录时无意义
};

// 遍历时对每一项的处置。
enum class MaiWalkAction {
    Continue,       // 接着走
    SkipDirectory,  // 这个目录不要进去（node_modules 这类靠它剪枝）
    Stop,           // 整个遍历到此为止
};

class MaiFileSystem {
public:
    // ── 查询 ────────────────────────────────────────────────────
    static bool exists(const MaiFilePath& path);
    static bool isDirectory(const MaiFilePath& path);
    // 拿不到大小时返回 false，size 不动。
    static bool fileSize(const MaiFilePath& path, std::uint64_t& size);

    // ── 读写 ────────────────────────────────────────────────────
    // 整个读进来。maxBytes 为 0 表示不限；超过就读到上限为止并把
    // truncated 置位——工具层要据此告诉模型"你看到的不是全部"。
    static MaiError readFile(const MaiFilePath& path, std::string& contents,
                             std::uint64_t maxBytes = 0, bool* truncated = nullptr);

    // 覆盖写。父目录不会自动创建，调用方自己先 createDirectories。
    static MaiError writeFile(const MaiFilePath& path, const std::string& contents);

    // ── 目录 ────────────────────────────────────────────────────
    // 逐级创建，已存在不算错。
    static MaiError createDirectories(const MaiFilePath& path);

    // 递归遍历。
    //
    // **不跟进目录符号链接**。跟进的话，一个指回上级的链接就能让遍历
    // 无限转下去——chromium 在 POSIX 上是靠记 (dev, ino) 防这个，
    // 我们直接不跟进，既简单又和 std::filesystem 的默认行为一致，
    // 换过来不会改变现有语义。
    //
    // 遍历顺序不保证，和 chromium 的 FileEnumerator 一样。glob 要排序的话
    // 自己排。
    static void walk(const MaiFilePath& root,
                     const std::function<MaiWalkAction(const MaiFileEntry&)>& visit);

    // ── 其它 ────────────────────────────────────────────────────
    // 解析成真实路径：消掉 "."/".."，并且**解析符号链接**。
    // 路径不存在时尽力而为——把存在的那一段解析掉，剩下的做词法规范化。
    // 失败返回空路径。
    static MaiFilePath resolve(const MaiFilePath& path);

    // 系统临时目录。测试用。
    static MaiFilePath temporaryDirectory();

    // 递归删除。测试收尾用；删不掉不报错，因为那时候已经没人在意了。
    static void removeRecursively(const MaiFilePath& path);
};
