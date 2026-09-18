#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

#include <json.hpp>

#include "MaiTool.h"

#include "MaiPathUtf8.h"

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── 输出上限 ────────────────────────────────────────────────────
// 工具输出会原样进上下文，所以每一个字节都要花 token、也都要占内存。
// grep 一个大仓库能返回几 MB —— 不设限的话内存、请求体、费用一起炸。
// 这是计划里的二号性能风险。
constexpr std::size_t kMaxOutputBytes = 64 * 1024;
constexpr std::size_t kMaxReadBytes = 256 * 1024;
constexpr int kMaxGrepMatches = 200;
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

std::string toRelativePath(const std::string& root, const fs::path& path) {
    std::error_code errorCode;
    const fs::path rel = fs::relative(path, MaiPathUtf8::fromUtf8(root), errorCode);
    // 一律返回 UTF-8。generic 形式统一用 / 分隔，这样模型看到的路径
    // 在三个平台上长得一样，它给回来的路径我们也认。
    return errorCode ? MaiPathUtf8::toUtf8(path) : MaiPathUtf8::toUtf8Generic(rel);
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

        std::error_code errorCode;
        const fs::path target = MaiPathUtf8::fromUtf8(resolved.path);
        if (!fs::exists(target, errorCode) || errorCode)
            return MaiToolResult::failure(
                MaiErrorCode::NotFound,
                "file does not exist: " + args.value("path", std::string{}));
        if (fs::is_directory(target, errorCode))
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "That is a directory, not a file. Use glob to list its contents.");

        std::ifstream in(MaiPathUtf8::fromUtf8(resolved.path), std::ios::binary);
        if (!in)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "could not open file for reading");

        const int offset = std::max(1, args.value("offset", 1));
        const int limit = std::max(1, args.value("limit", 500));

        std::string out;
        std::string line;
        int lineno = 0;
        int emitted = 0;
        bool truncated = false;
        while (std::getline(in, line)) {
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

        std::error_code errorCode;
        const fs::path path = MaiPathUtf8::fromUtf8(resolved.path);
        if (path.has_parent_path()) {
            fs::create_directories(path.parent_path(), errorCode);
            if (errorCode)
                return MaiToolResult::failure(
                    MaiErrorCode::Internal,
                    "could not create parent directory: " + errorCode.message());
        }
        std::ofstream out(MaiPathUtf8::fromUtf8(resolved.path), std::ios::binary | std::ios::trunc);
        if (!out)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "could not open file for writing");
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) return MaiToolResult::failure(MaiErrorCode::Internal, "write failed");
        out.close();

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

        std::vector<std::string> hits;
        std::error_code errorCode;
        fs::recursive_directory_iterator it(
            MaiPathUtf8::fromUtf8(base), fs::directory_options::skip_permission_denied, errorCode);
        if (errorCode)
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "could not read directory: " + errorCode.message());

        for (; it != fs::recursive_directory_iterator(); it.increment(errorCode)) {
            if (errorCode) break;
            if (context.isCanceled()) break;
            if (it->is_directory(errorCode)) {
                if (shouldSkipDirectory(MaiPathUtf8::toUtf8(it->path().filename())))
                    it.disable_recursion_pending();
                continue;
            }
            const std::string rel = toRelativePath(context.root, it->path());
            if (globMatch(pattern, rel) ||
                globMatch(pattern, MaiPathUtf8::toUtf8(it->path().filename()))) {
                hits.push_back(rel);
                if (static_cast<int>(hits.size()) >= kMaxGlobResults) break;
            }
        }

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
        std::error_code errorCode;
        fs::recursive_directory_iterator it(
            MaiPathUtf8::fromUtf8(base), fs::directory_options::skip_permission_denied, errorCode);
        if (errorCode)
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "could not read directory: " + errorCode.message());

        for (; it != fs::recursive_directory_iterator() && matches < kMaxGrepMatches;
             it.increment(errorCode)) {
            if (errorCode) break;
            if (context.isCanceled()) break;
            if (it->is_directory(errorCode)) {
                if (shouldSkipDirectory(MaiPathUtf8::toUtf8(it->path().filename())))
                    it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(errorCode)) continue;
            const std::string rel = toRelativePath(context.root, it->path());
            if (hasFilter && !globMatch(globPattern, rel) &&
                !globMatch(globPattern, MaiPathUtf8::toUtf8(it->path().filename())))
                continue;

            // 太大的文件跳过：多半是二进制或产物，搜了也没意义还很慢。
            const auto size = fs::file_size(it->path(), errorCode);
            if (errorCode || size > 2u * 1024 * 1024) continue;

            std::ifstream in(it->path(), std::ios::binary);
            if (!in) continue;
            std::string line;
            int lineno = 0;
            while (std::getline(in, line) && matches < kMaxGrepMatches) {
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
                out += rel;
                out += ":";
                out += std::to_string(lineno);
                out += ": ";
                out += line;
                out += "\n";
                ++matches;
                if (out.size() > kMaxOutputBytes) break;
            }
            if (out.size() > kMaxOutputBytes) break;
        }

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
