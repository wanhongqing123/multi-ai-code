#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

// ── 路径安全 ────────────────────────────────────────────────────
//
// 这是工具层唯一的安全边界。模型会试着越界——有时是它自己想看看../.env，有时是被提示词注入诱导的。
// 所以这里按"默认拒绝"来写。
//
// 三步，缺一不可：
//
//   1. 相对路径按 root 拼出来（绝对路径原样拿去判断）。
//   2. 两边都过 MaiFileSystem::resolve——它会消掉 ".."，并且**解析符号
//      链接**。只做词法规范化是不够的：root 里放一个指向 C:\ 的链接或
//      联接，词法上看它就在 root 里面。
//   3. 逐段比对。不能比字符串前缀——"/server/app-secrets" 是
//      "/server/app" 的前缀，却不在那个目录里。MaiFilePath::isParentOf
//      做的就是逐段比，Windows 上还不分大小写。
//
// 不存在的路径也要能过（write 要创建新文件）。resolve 对不存在的路径是尽力而为：
// 把存在的那一段解析掉，剩下的做词法规范化。
std::string maiResolvePathWithinRoot(const std::string& root, const std::string& candidate) {
    if (root.empty() || candidate.empty()) return {};

    const MaiFilePath rootPath = MaiFileSystem::resolve(MaiFilePath::fromUtf8(root));
    if (rootPath.isEmpty()) return {};

    MaiFilePath target = MaiFilePath::fromUtf8(candidate);
    if (!target.isAbsolute()) target = rootPath.append(target);

    const MaiFilePath resolved = MaiFileSystem::resolve(target);
    if (resolved.isEmpty()) return {};

    // root 自己也算"在 root 之内"：工具允许对工作目录本身操作（比如 glob 的起点就是它）。
    // isParentOf 对相等返回 false，所以这里要单独放行。
    if (resolved != rootPath && !rootPath.isParentOf(resolved)) return {};

    return resolved.toUtf8();
}
