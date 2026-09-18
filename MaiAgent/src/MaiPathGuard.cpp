#include "MaiTool.h"

#include <filesystem>
#include <system_error>

#include "MaiPathUtf8.h"

namespace fs = std::filesystem;

// ── 路径安全 ────────────────────────────────────────────────────
//
// 这是工具层唯一的安全边界。模型会试着越界——有时是它自己想看看
// ../.env，有时是被提示词注入诱导的。所以这里按"默认拒绝"来写。
//
// 用 weakly_canonical 而不是 canonical：目标文件可能还不存在（write 要创建它），
// canonical 在那种情况下直接失败。weakly_canonical 会解析已存在的那一段，
// 对不存在的部分做词法规范化——`..` 仍然会被消掉，这是我们要的。
//
// 已存在的路径还要再用 canonical 过一遍：weakly_canonical 在某些平台上
// 不解析符号链接，而符号链接是最容易被忽略的越界手段——
// root 里放一个指向 C:\ 的链接就绕过去了。
std::string maiResolvePathWithinRoot(const std::string& root, const std::string& candidate) {
    if (root.empty() || candidate.empty()) return {};

    std::error_code errorCode;
    // 一律走 path_from_utf8 / path_to_utf8。直接用 fs::path(std::string) 会按
    // 当前 ANSI 代码页解释（Windows 中文环境是 GBK），中文路径立刻出问题。
    const fs::path rootPath = fs::weakly_canonical(MaiPathUtf8::fromUtf8(root), errorCode);
    if (errorCode) return {};

    fs::path target = MaiPathUtf8::fromUtf8(candidate);
    // 相对路径按 root 解析；绝对路径原样拿去判断是否落在 root 内。
    if (target.is_relative()) target = rootPath / target;

    fs::path resolved = fs::weakly_canonical(target, errorCode);
    if (errorCode) return {};

    // 存在的话再解一次符号链接：weakly_canonical 在某些平台上不解析它们，
    // 而 root 里放一个指向 C:\ 的链接就能绕过整个检查。
    if (fs::exists(resolved, errorCode) && !errorCode) {
        const fs::path real = fs::canonical(resolved, errorCode);
        if (!errorCode) resolved = real;
    }

    // 逐段比对，不能用字符串前缀——前缀判断会让 /server/app-secrets 通过
    // 对 /server/app 的检查。
    auto rootSegment = rootPath.begin();
    auto resolvedSegment = resolved.begin();
    while (rootSegment != rootPath.end()) {
        if (resolvedSegment == resolved.end()) return {};  // 比 root 还短，肯定在外面
        if (*rootSegment != *resolvedSegment) return {};
        ++rootSegment;
        ++resolvedSegment;
    }
    return MaiPathUtf8::toUtf8(resolved);
}
