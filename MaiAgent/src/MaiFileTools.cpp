#include <algorithm>
#include <cstdint>
#include <regex>
#include <sstream>
#include <system_error>

#include <json.hpp>

#include "MaiTool.h"

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using json = nlohmann::json;

// ── 输出上限 ────────────────────────────────────────────────────
// 工具输出会原样进上下文，所以每一个字节都要花 token、也都要占内存。
// grep 一个大仓库能返回几 MB —— 不设限的话内存、请求体、费用一起炸。
// 这是计划里的二号性能风险。
constexpr std::size_t kMaxOutputBytes = 64 * 1024;
constexpr std::size_t kMaxReadBytes = 256 * 1024;
constexpr int kMaxGrepMatches = 200;
// 单个文件最多读这么多。几百兆的文件读全了既没意义又会把内存吃光；
// 截断了会告诉模型，它可以用 offset 继续读。
constexpr std::uint64_t kMaxFileBytes = 8u * 1024 * 1024;

constexpr int kMaxGlobResults = 300;

// 截断时要落在 UTF-8 字符边界上，不然会切出半个汉字，
// 后面 JSON 序列化会失败或者产生乱码。
void truncateUtf8(std::string& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    text.resize(cut);
}

json parseArguments(const std::string& raw) {
    const json parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    return parsed.is_object() ? parsed : json::object();
}

// 把 root 之外的路径挡掉，并给模型一句它能据此改正的话。
struct Resolved {
    std::string path;
    MaiToolResult error;
    bool ok = false;
};

Resolved resolveOrFail(const MaiToolContext& context, const std::string& rawPath) {
    if (rawPath.empty())
        return {
            {},
            MaiToolResult::failure(MaiErrorCode::InvalidInput, "missing required parameter: path"),
            false};
    const std::string resolved = maiResolvePathWithinRoot(context.root, rawPath);
    if (resolved.empty()) {
        return {{},
                MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "Path is outside the working directory and was rejected: " + rawPath +
                        ". Only files inside the working directory are accessible."),
                false};
    }
    return {resolved, {}, true};
}

// glob 匹配。**不用 std::regex。**
//
// 第一版是把 glob 转成正则再交给 std::regex，结果整个进程崩了
// （STATUS_STACK_BUFFER_OVERRUN）。原因是 `**/*.cpp` 会转成
// `.*[^/\\]*\.cpp` 这种形状，两个贪婪量词挨着会产生灾难性回溯，
// 而 MSVC 的 std::regex 是递归实现的，回溯深度直接把栈打穿。
//
// 手写的这个是迭代式的：记住最近一次 `*` 的位置，失配就回到那里让 `*`
// 多吃一个字符。最坏 O(n*m)，不递归、不分配、没有栈风险。
//
// 语义：
//   *   匹配任意字符，但不跨路径分隔符
//   **  匹配任意字符，跨分隔符
//   ?   匹配单个非分隔符字符
// 大小写不敏感，只对 ASCII 做折叠——中文没有大小写，不需要处理。
bool isSeparator(char character) {
    return character == '/' || character == '\\';
}

char fold(char character) {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a')
                                                  : character;
}

bool globMatch(const std::string& pattern, const std::string& text) {
    std::size_t cursor = 0, textCursor = 0;
    std::size_t starPattern = std::string::npos, starText = 0;
    bool starCrossesSeparator = false;

    while (textCursor < text.size()) {
        if (cursor < pattern.size() && pattern[cursor] == '*') {
            const bool doubled = (cursor + 1 < pattern.size() && pattern[cursor + 1] == '*');
            starPattern = cursor;
            starCrossesSeparator = doubled;
            cursor += doubled ? 2 : 1;
            // `**/` 里的那个分隔符是可选的：`**/*.cpp` 也该匹配根目录下的 a.cpp
            if (doubled && cursor < pattern.size() && isSeparator(pattern[cursor])) ++cursor;
            starText = textCursor;
            continue;
        }
        if (cursor < pattern.size() &&
            (fold(pattern[cursor]) == fold(text[textCursor]) ||
             (pattern[cursor] == '?' && !isSeparator(text[textCursor])) ||
             (isSeparator(pattern[cursor]) && isSeparator(text[textCursor])))) {
            ++cursor;
            ++textCursor;
            continue;
        }
        // 失配：退回最近的 `*`，让它多吃一个字符。
        if (starPattern != std::string::npos) {
            // 单星不跨分隔符，遇到分隔符就彻底失败。
            if (!starCrossesSeparator && isSeparator(text[starText])) return false;
            ++starText;
            textCursor = starText;
            cursor = starPattern + (starCrossesSeparator ? 2 : 1);
            if (starCrossesSeparator && cursor < pattern.size() && isSeparator(pattern[cursor]))
                ++cursor;
            continue;
        }
        return false;
    }
    // 文本吃完了，剩下的模式必须全是 `*`
    while (cursor < pattern.size() && pattern[cursor] == '*') ++cursor;
    return cursor == pattern.size();
}

// 跳过这些目录：它们体量巨大而且几乎肯定不是模型要找的东西。
// 不跳过的话 glob 一个前端仓库会在 node_modules 里走几十万个文件。
bool shouldSkipDirectory(const std::string& name) {
    static const char* kSkip[] = {".git", "node_modules", "target",      "build", "dist",
                                  "out",  ".venv",        "__pycache__", ".cache"};
    for (const char* text : kSkip)
        if (name == text) return true;
    return false;
}

// path 相对于 root 的写法。算不出来（不在 root 下）就返回完整路径。
//
// 自己按段算，不调系统的 PathRelativePathToW：那个 API 在两边不同盘时
// 行为古怪，而我们这里 path 一定在 root 之内（调用方已经过了安全检查），
// 逐段砍掉公共前缀就够了。
//
// 一律返回 generic 形式（'/' 分隔）的 UTF-8：模型看到的路径在三个平台上
// 长得一样，它给回来的我们也认。
std::string toRelativePath(const std::string& root, const MaiFilePath& path) {
    const MaiFilePath rootPath = MaiFilePath::fromUtf8(root);
    const auto rootParts = rootPath.components();
    const auto pathParts = path.components();
    if (pathParts.size() <= rootParts.size()) return path.toGenericUtf8();

    MaiFilePath relative;
    for (std::size_t i = rootParts.size(); i < pathParts.size(); ++i)
        relative = relative.append(MaiFilePath(pathParts[i]));
    return relative.isEmpty() ? path.toGenericUtf8() : relative.toGenericUtf8();
}

// ── read ────────────────────────────────────────────────────────
class ReadTool final : public MaiTool {
public:
    std::string name() const override {
        return "read";
    }
    std::string description() const override {
        return "Read a file inside the working directory. Returns the text with line numbers "
               "so you can refer to specific lines later.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"File path relative to the working directory"},)"
               R"("offset":{"type":"integer","description":"1-based line to start from, default 1"},)"
               R"("limit":{"type":"integer","description":"Maximum number of lines to read, default 500"}},)"
               R"("required":["path"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        auto resolved = resolveOrFail(context, args.value("path", std::string{}));
        if (!resolved.ok) return resolved.error;

        const MaiFilePath target = MaiFilePath::fromUtf8(resolved.path);
        if (!MaiFileSystem::exists(target))
            return MaiToolResult::failure(
                MaiErrorCode::NotFound,
                "file does not exist: " + args.value("path", std::string{}));
        if (MaiFileSystem::isDirectory(target))
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "That is a directory, not a file. Use glob to list its contents.");

        // 一次读进来，再自己按行切。以前用 std::getline 一行行读，
        // 每行一次系统调用；一次读完再切，大文件上差别明显。
        // 读多少有上限，免得一个几百兆的文件把内存吃光。
        std::string blob;
        bool readTruncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(target, blob, kMaxFileBytes, &readTruncated);
        if (readError.hasError())
            return MaiToolResult::failure(readError.code(), readError.message());

        const int offset = std::max(1, args.value("offset", 1));
        const int limit = std::max(1, args.value("limit", 500));

        std::string out;
        std::string line;
        int lineno = 0;
        int emitted = 0;
        bool truncated = readTruncated;
        std::size_t cursor = 0;
        while (cursor <= blob.size()) {
            const std::size_t newlineAt = blob.find('\n', cursor);
            if (newlineAt == std::string::npos) {
                if (cursor >= blob.size()) break;
                line = blob.substr(cursor);
                cursor = blob.size() + 1;
            } else {
                line = blob.substr(cursor, newlineAt - cursor);
                cursor = newlineAt + 1;
            }
            ++lineno;
            if (lineno < offset) continue;
            if (emitted >= limit) {
                truncated = true;
                break;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out += std::to_string(lineno);
            out += "\t";
            out += line;
            out += "\n";
            ++emitted;
            if (out.size() > kMaxReadBytes) {
                truncated = true;
                break;
            }
        }

        if (out.empty()) {
            return MaiToolResult::success(lineno == 0 ? "(empty file)"
                                                      : "(no content at or after line " +
                                                            std::to_string(offset) + ")");
        }
        truncateUtf8(out, kMaxReadBytes);
        if (truncated) out += "\n...Output truncated. Use the offset parameter to read further.";
        return MaiToolResult::success(std::move(out), truncated);
    }
};

// ── write ───────────────────────────────────────────────────────
class WriteTool final : public MaiTool {
public:
    std::string name() const override {
        return "write";
    }
    std::string description() const override {
        return "Write content to a file inside the working directory, replacing whatever was "
               "there. Parent directories are created as needed.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"File path relative to the working directory"},)"
               R"("content":{"type":"string","description":"The full content to write"}},)"
               R"("required":["path","content"]})";
    }
    // 会改文件。M4 的闸门就位后这里会真正拦一道。
    bool requiresApproval() const override {
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        auto resolved = resolveOrFail(context, args.value("path", std::string{}));
        if (!resolved.ok) return resolved.error;
        if (!args.contains("content") || !args["content"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: content");

        const std::string content = args["content"].get<std::string>();

        const MaiFilePath path = MaiFilePath::fromUtf8(resolved.path);
        const MaiFilePath parent = path.dirName();
        if (!parent.isEmpty() && parent != path) {
            const MaiError error = MaiFileSystem::createDirectories(parent);
            if (error.hasError())
                return MaiToolResult::failure(
                    error.code(), "could not create parent directory: " + error.message());
        }
        const MaiError error = MaiFileSystem::writeFile(path, content);
        if (error.hasError()) return MaiToolResult::failure(error.code(), error.message());

        return MaiToolResult::success("Wrote " + toRelativePath(context.root, path) + " (" +
                                      std::to_string(content.size()) + " bytes)");
    }
};

// ── glob ────────────────────────────────────────────────────────
class GlobTool final : public MaiTool {
public:
    std::string name() const override {
        return "glob";
    }
    std::string description() const override {
        return "Find files inside the working directory by name pattern. Supports * and ?, "
               "and ** to cross directory boundaries.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("pattern":{"type":"string","description":"For example **/*.cpp or src/*.h"},)"
               R"("path":{"type":"string","description":"Subdirectory to search from, defaults to the working directory root"}},)"
               R"("required":["pattern"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string pattern = args.value("pattern", std::string{});
        if (pattern.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: pattern");

        std::string base = context.root;
        if (args.contains("path") && args["path"].is_string() &&
            !args["path"].get<std::string>().empty()) {
            auto resolved = resolveOrFail(context, args["path"].get<std::string>());
            if (!resolved.ok) return resolved.error;
            base = resolved.path;
        }

        const MaiFilePath basePath = MaiFilePath::fromUtf8(base);
        if (!MaiFileSystem::isDirectory(basePath))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "could not read directory: " + base);

        std::vector<std::string> hits;
        MaiFileSystem::walk(basePath, [&](const MaiFileEntry& entry) {
            if (context.isCanceled()) return MaiWalkAction::Stop;
            if (entry.isDirectory) {
                return shouldSkipDirectory(entry.nameUtf8) ? MaiWalkAction::SkipDirectory
                                                           : MaiWalkAction::Continue;
            }
            const std::string relative = toRelativePath(context.root, entry.path);
            if (globMatch(pattern, relative) || globMatch(pattern, entry.nameUtf8)) {
                hits.push_back(relative);
                if (static_cast<int>(hits.size()) >= kMaxGlobResults) return MaiWalkAction::Stop;
            }
            return MaiWalkAction::Continue;
        });

        if (hits.empty()) return MaiToolResult::success("No files match " + pattern);
        std::sort(hits.begin(), hits.end());
        std::string out;
        for (const auto& hit : hits) {
            out += hit;
            out += "\n";
        }
        const bool truncated = static_cast<int>(hits.size()) >= kMaxGlobResults;
        if (truncated)
            out += "...Showing the first " + std::to_string(kMaxGlobResults) +
                   " matches only. Use a more specific pattern to narrow the search.";
        return MaiToolResult::success(std::move(out), truncated);
    }
};

// ── grep ────────────────────────────────────────────────────────
class GrepTool final : public MaiTool {
public:
    std::string name() const override {
        return "grep";
    }
    std::string description() const override {
        return "Search file contents inside the working directory with a regular expression. "
               "Returns the file, line number and the whole matching line.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("pattern":{"type":"string","description":"Regular expression"},)"
               R"("path":{"type":"string","description":"Subdirectory to search from, defaults to the working directory root"},)"
               R"("glob":{"type":"string","description":"Only search files matching this pattern, for example *.cpp"}},)"
               R"("required":["pattern"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string pattern = args.value("pattern", std::string{});
        if (pattern.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: pattern");

        std::string base = context.root;
        if (args.contains("path") && args["path"].is_string() &&
            !args["path"].get<std::string>().empty()) {
            auto resolved = resolveOrFail(context, args["path"].get<std::string>());
            if (!resolved.ok) return resolved.error;
            base = resolved.path;
        }

        std::regex re;
        try {
            re = std::regex(pattern, std::regex::ECMAScript);
        } catch (const std::regex_error& regexError) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                std::string("could not parse the regular expression: ") + regexError.what());
        }

        const std::string globPattern = args.value("glob", std::string{});
        const bool hasFilter = !globPattern.empty();

        std::string out;
        int matches = 0;

        const MaiFilePath basePath = MaiFilePath::fromUtf8(base);
        if (!MaiFileSystem::isDirectory(basePath))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "could not read directory: " + base);

        MaiFileSystem::walk(basePath, [&](const MaiFileEntry& entry) {
            if (context.isCanceled() || matches >= kMaxGrepMatches) return MaiWalkAction::Stop;
            if (entry.isDirectory) {
                return shouldSkipDirectory(entry.nameUtf8) ? MaiWalkAction::SkipDirectory
                                                           : MaiWalkAction::Continue;
            }
            const std::string relative = toRelativePath(context.root, entry.path);
            if (hasFilter && !globMatch(globPattern, relative) &&
                !globMatch(globPattern, entry.nameUtf8))
                return MaiWalkAction::Continue;

            // 太大的文件跳过：多半是二进制或产物，搜了也没意义还很慢。
            // 大小是遍历时顺路拿到的，不用再 stat 一次。
            if (entry.size > 2u * 1024 * 1024) return MaiWalkAction::Continue;

            std::string blob;
            if (MaiFileSystem::readFile(entry.path, blob).hasError())
                return MaiWalkAction::Continue;

            std::string line;
            int lineno = 0;
            std::size_t cursor = 0;
            while (cursor <= blob.size() && matches < kMaxGrepMatches) {
                const std::size_t newlineAt = blob.find('\n', cursor);
                if (newlineAt == std::string::npos) {
                    if (cursor >= blob.size()) break;
                    line = blob.substr(cursor);
                    cursor = blob.size() + 1;
                } else {
                    line = blob.substr(cursor, newlineAt - cursor);
                    cursor = newlineAt + 1;
                }
                ++lineno;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                // 含 NUL 的当二进制跳过整个文件。
                if (line.find('\0') != std::string::npos) break;
                // 先截断再匹配。用户给的正则可能有灾难性回溯，行越长越容易把栈打穿——
                // glob 那边已经因此崩过一次，这里不重蹈覆辙。
                if (line.size() > 400) {
                    std::size_t cut = 400;
                    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
                    line.resize(cut);
                }
                if (!std::regex_search(line, re)) continue;
                out += relative;
                out += ":";
                out += std::to_string(lineno);
                out += ": ";
                out += line;
                out += "\n";
                ++matches;
                if (out.size() > kMaxOutputBytes) break;
            }
            return out.size() > kMaxOutputBytes ? MaiWalkAction::Stop : MaiWalkAction::Continue;
        });

        if (matches == 0) return MaiToolResult::success("No content matches " + pattern);
        const bool truncated = matches >= kMaxGrepMatches || out.size() > kMaxOutputBytes;
        truncateUtf8(out, kMaxOutputBytes);
        if (truncated)
            out +=
                "\n...Too many matches; showing the first ones only. Use a more specific "
                "regular expression, or add the glob parameter to narrow the search.";
        return MaiToolResult::success(std::move(out), truncated);
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiReadTool() {
    return std::make_unique<ReadTool>();
}
std::unique_ptr<MaiTool> makeMaiWriteTool() {
    return std::make_unique<WriteTool>();
}
std::unique_ptr<MaiTool> makeMaiGlobTool() {
    return std::make_unique<GlobTool>();
}
std::unique_ptr<MaiTool> makeMaiGrepTool() {
    return std::make_unique<GrepTool>();
}
