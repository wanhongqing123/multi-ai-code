#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__ANDROID__)
#include <iconv.h>
#endif

#include <json.hpp>

#include "MaiFileTools.h"

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using json = nlohmann::json;

// ── 输出上限 ────────────────────────────────────────────────────
// 工具输出会原样进上下文，所以每一个字节都要花 token、也都要占内存。
// grep 一个大仓库能返回几 MB —— 不设限的话内存、请求体、费用一起炸。这是计划里的二号性能风险。
constexpr std::size_t kMaxOutputBytes = 64 * 1024;
constexpr std::size_t kMaxReadBytes = 256 * 1024;
constexpr int kMaxGrepMatches = 200;
// 单个文件最多读这么多。几百兆的文件读全了既没意义又会把内存吃光；截断了会告诉模型，
// 它可以用 offset 继续读。
constexpr std::uint64_t kMaxFileBytes = 8u * 1024 * 1024;

constexpr int kMaxGlobResults = 300;
constexpr int kMaxGlobVisitedEntries = 50000;
constexpr auto kMaxGlobDuration = std::chrono::seconds(5);

// 截断时要落在 UTF-8 字符边界上，不然会切出半个汉字，后面 JSON 序列化会失败或者产生乱码。
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

std::string stringArgument(const json& arguments, const char* name) {
    const auto value = arguments.find(name);
    return value != arguments.end() && value->is_string() ? value->get<std::string>()
                                                          : std::string{};
}

bool isValidUtf8(const std::string& text) {
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first < 0x80) {
            ++index;
            continue;
        }
        int width = 0;
        if (first >= 0xC2 && first <= 0xDF)
            width = 2;
        else if (first >= 0xE0 && first <= 0xEF)
            width = 3;
        else if (first >= 0xF0 && first <= 0xF4)
            width = 4;
        else
            return false;
        if (index + width > text.size()) return false;
        const unsigned char second = static_cast<unsigned char>(text[index + 1]);
        if (second < 0x80 || second > 0xBF || (first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second > 0x9F) || (first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second > 0x8F))
            return false;
        for (int offset = 2; offset < width; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if (next < 0x80 || next > 0xBF) return false;
        }
        index += static_cast<std::size_t>(width);
    }
    return true;
}

bool looksBinary(const std::string& text) {
    std::size_t controls = 0;
    for (const unsigned char byte : text) {
        if (byte == 0) return true;
        if (byte < 0x09 || (byte > 0x0D && byte < 0x20)) ++controls;
    }
    return !text.empty() && controls > text.size() / 100;
}

void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

MaiResult<std::string> decodeUtf16(const std::string& bytes, bool littleEndian) {
    std::size_t offset = 0;
    if (bytes.size() >= 2 && ((littleEndian && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                               static_cast<unsigned char>(bytes[1]) == 0xFE) ||
                              (!littleEndian && static_cast<unsigned char>(bytes[0]) == 0xFE &&
                               static_cast<unsigned char>(bytes[1]) == 0xFF)))
        offset = 2;
    if ((bytes.size() - offset) % 2 != 0)
        return {MaiErrorCode::InvalidInput, "UTF-16 file ends with an incomplete code unit"};
    std::string output;
    output.reserve(bytes.size());
    auto unitAt = [&](std::size_t at) {
        const auto first = static_cast<unsigned char>(bytes[at]);
        const auto second = static_cast<unsigned char>(bytes[at + 1]);
        return static_cast<std::uint32_t>(littleEndian ? first | (second << 8)
                                                       : (first << 8) | second);
    };
    for (; offset < bytes.size(); offset += 2) {
        std::uint32_t codePoint = unitAt(offset);
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (offset + 3 >= bytes.size())
                return {MaiErrorCode::InvalidInput, "UTF-16 file ends with a high surrogate"};
            const std::uint32_t low = unitAt(offset + 2);
            if (low < 0xDC00 || low > 0xDFFF)
                return {MaiErrorCode::InvalidInput, "UTF-16 file has an invalid surrogate pair"};
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            offset += 2;
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
            return {MaiErrorCode::InvalidInput, "UTF-16 file has an unmatched low surrogate"};
        }
        appendUtf8(output, codePoint);
    }
    if (looksBinary(output))
        return {MaiErrorCode::InvalidInput, "file contains binary control bytes"};
    return output;
}

std::string normalizedEncoding(const std::string& name) {
    std::string result = name.empty() ? "auto" : name;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return character == '_' ? '-' : static_cast<char>(std::tolower(character));
    });
    return result;
}

MaiResult<std::string> decodeLegacy(const std::string& bytes, const std::string& encoding,
                                    const MaiToolContext& context) {
    if (context.decodeText) return context.decodeText(bytes, encoding);
#if defined(_WIN32)
    UINT codePage = 0;
    if (encoding == "auto" || encoding == "system")
        codePage = CP_ACP;
    else if (encoding == "gb18030")
        codePage = 54936;
    else if (encoding == "gbk" || encoding == "cp936")
        codePage = 936;
    else if (encoding == "windows-1252")
        codePage = 1252;
    else if (encoding == "latin1" || encoding == "iso-8859-1")
        codePage = 28591;
    else if (encoding == "shift-jis" || encoding == "shift-jis-2004")
        codePage = 932;
    else
        return {MaiErrorCode::InvalidInput, "unsupported file encoding: " + encoding};
    const int length = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(),
                                             static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0)
        return {MaiErrorCode::InvalidInput, "file could not be decoded as " + encoding};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(),
                          static_cast<int>(bytes.size()), wide.data(), length);
    const int utf8Length =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return {MaiErrorCode::InvalidInput, "file could not be converted to UTF-8"};
    std::string output(static_cast<std::size_t>(utf8Length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, output.data(), utf8Length, nullptr,
                          nullptr);
    return output;
#elif !defined(__ANDROID__)
    const std::string source = encoding == "auto" ? "GB18030" : encoding;
    iconv_t converter = ::iconv_open("UTF-8", source.c_str());
    if (converter == reinterpret_cast<iconv_t>(-1))
        return {MaiErrorCode::InvalidInput, "unsupported file encoding: " + encoding};
    std::string output(bytes.size() * 4 + 4, '\0');
    char* input = const_cast<char*>(bytes.data());
    std::size_t remaining = bytes.size();
    char* destination = output.data();
    std::size_t capacity = output.size();
    const std::size_t converted = ::iconv(converter, &input, &remaining, &destination, &capacity);
    ::iconv_close(converter);
    if (converted == static_cast<std::size_t>(-1) || remaining != 0)
        return {MaiErrorCode::InvalidInput, "file could not be decoded as " + source};
    output.resize(output.size() - capacity);
    return output;
#else
    return {MaiErrorCode::NotConfigured, "this Android host has no legacy text decoder configured"};
#endif
}

MaiResult<std::string> decodeFileText(const std::string& bytes, const std::string& requested,
                                      const MaiToolContext& context) {
    const std::string encoding = normalizedEncoding(requested);
    if (encoding == "utf-16le" || encoding == "utf16le") return decodeUtf16(bytes, true);
    if (encoding == "utf-16be" || encoding == "utf16be") return decodeUtf16(bytes, false);
    if (encoding == "auto" && bytes.size() >= 2) {
        if (static_cast<unsigned char>(bytes[0]) == 0xFF &&
            static_cast<unsigned char>(bytes[1]) == 0xFE)
            return decodeUtf16(bytes, true);
        if (static_cast<unsigned char>(bytes[0]) == 0xFE &&
            static_cast<unsigned char>(bytes[1]) == 0xFF)
            return decodeUtf16(bytes, false);
    }
    if (encoding == "auto" || encoding == "utf-8" || encoding == "utf8") {
        const std::size_t bom = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                                        static_cast<unsigned char>(bytes[1]) == 0xBB &&
                                        static_cast<unsigned char>(bytes[2]) == 0xBF
                                    ? 3
                                    : 0;
        const std::string utf8 = bytes.substr(bom);
        if (isValidUtf8(utf8) && !looksBinary(utf8)) return utf8;
        if (encoding != "auto")
            return {MaiErrorCode::InvalidInput,
                    "file is not valid UTF-8; retry with an explicit encoding"};
    }
    if (looksBinary(bytes))
        return {MaiErrorCode::InvalidInput,
                "file looks binary or uses UTF-16 without a BOM; specify encoding explicitly"};
    MaiResult<std::string> decoded = decodeLegacy(bytes, encoding, context);
    if (!decoded) return decoded;
    if (!isValidUtf8(decoded.value()) || looksBinary(decoded.value()))
        return {MaiErrorCode::InvalidInput, "decoded file is not valid UTF-8 text"};
    return decoded;
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
    const std::string resolved = context.resolvePath(rawPath);
    if (resolved.empty()) {
        return {
            {},
            MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                   "Path is outside the area accessible to this host: " + rawPath),
            false};
    }
    return {resolved, {}, true};
}

// glob 匹配。**不用 std::regex。**
//
// 第一版是把 glob 转成正则再交给 std::regex，结果整个进程崩了
// （STATUS_STACK_BUFFER_OVERRUN）。原因是 `**/*.cpp` 会转成`.*[^/\\]*\.cpp` 这种形状，
// 两个贪婪量词挨着会产生灾难性回溯，
// 而 MSVC 的 std::regex 是递归实现的，回溯深度直接把栈打穿。
//
// 手写的这个是迭代式的：记住最近一次 `*` 的位置，失配就回到那里让 `*`多吃一个字符。最坏 O(n*m)，
// 不递归、不分配、没有栈风险。
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

// A small, bounded brace expansion covers common extension lists without letting a model
// accidentally create an unbounded number of patterns.
bool expandGlobBraces(const std::string& pattern, std::vector<std::string>& expanded) {
    if (pattern.size() > 512) return false;
    expanded = {pattern};
    for (std::size_t index = 0; index < expanded.size(); ++index) {
        const std::size_t open = expanded[index].find('{');
        if (open == std::string::npos) {
            if (expanded[index].find('}') != std::string::npos) return false;
            continue;
        }
        const std::size_t close = expanded[index].find('}', open + 1);
        if (close == std::string::npos || expanded[index].find('{', open + 1) < close) return false;
        const std::string prefix = expanded[index].substr(0, open);
        const std::string suffix = expanded[index].substr(close + 1);
        const std::string alternatives = expanded[index].substr(open + 1, close - open - 1);
        expanded.erase(expanded.begin() + static_cast<std::ptrdiff_t>(index));
        std::size_t start = 0;
        do {
            const std::size_t comma = alternatives.find(',', start);
            const std::string choice = alternatives.substr(start, comma - start);
            if (choice.empty() || choice.find('{') != std::string::npos ||
                choice.find('}') != std::string::npos || expanded.size() >= 32)
                return false;
            expanded.push_back(prefix + choice + suffix);
            if (comma == std::string::npos) break;
            start = comma + 1;
        } while (true);
        index = static_cast<std::size_t>(-1);
    }
    return true;
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
// 自己按段算，不调系统的 PathRelativePathToW：那个 API 在两边不同盘时行为古怪，
// root 内的路径逐段裁成相对路径；宿主允许 root 外访问时保留完整绝对路径。
//
// 一律返回 generic 形式（'/' 分隔）的 UTF-8：模型看到的路径在三个平台上长得一样，
// 它给回来的我们也认。
std::string toRelativePath(const std::string& root, const MaiFilePath& path) {
    const MaiFilePath rootPath = MaiFilePath::fromUtf8(root);
    if (path != rootPath && !rootPath.isParentOf(path)) return path.toGenericUtf8();
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
        return "Read a text file as UTF-8 with line numbers. UTF-8 and BOM-marked UTF-16 are "
               "detected automatically; for ambiguous legacy text, set encoding explicitly "
               "(for example gb18030, gbk, or windows-1252). Relative paths use the session "
               "working directory.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"Absolute path or path relative to the working directory"},)"
               R"("encoding":{"type":"string","description":"Source text encoding; default auto. Examples: utf-8, utf-16le, utf-16be, gb18030, gbk, windows-1252"},)"
               R"("offset":{"type":"integer","description":"1-based line to start from, default 1"},)"
               R"("limit":{"type":"integer","description":"Maximum number of lines to read, default 500"}},)"
               R"("required":["path"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        if (args.contains("encoding") && !args["encoding"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "encoding must be a string");
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return resolved.error;

        const MaiFilePath target = MaiFilePath::fromUtf8(resolved.path);
        if (!MaiFileSystem::exists(target))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "file does not exist: " + stringArgument(args, "path"));
        if (MaiFileSystem::isDirectory(target))
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "That is a directory, not a file. Use glob to list its contents.");

        // 一次读进来，再自己按行切。以前用 std::getline 一行行读，
        // 每行一次系统调用；一次读完再切，大文件上差别明显。读多少有上限，
        // 免得一个几百兆的文件把内存吃光。
        std::string blob;
        bool readTruncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(target, blob, kMaxFileBytes, &readTruncated);
        if (readError.hasError())
            return MaiToolResult::failure(readError.code(), readError.message());
        const MaiResult<std::string> decoded =
            decodeFileText(blob, stringArgument(args, "encoding"), context);
        if (!decoded)
            return MaiToolResult::failure(decoded.error().code(), decoded.error().message());
        blob = decoded.value();

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

// ── explicit file and directory mutations ─────────────────────
class CreateFileTool final : public MaiTool {
public:
    std::string name() const override {
        return "create_file";
    }
    std::string description() const override {
        return "Create a new empty file without overwriting an existing file. Missing parent "
               "directories are created. Use write when the file needs content.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string","description":"Absolute path or path relative to the working directory"}},"required":["path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const std::string path =
            context.resolvePath(stringArgument(parseArguments(argumentsJson), "path"));
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"file:" + path};
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        if (args.size() != 1 || !args.contains("path") || !args["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "create_file requires one string parameter named path");
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return resolved.error;
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved.path);
        if (MaiFileSystem::exists(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "path already exists; nothing was overwritten");
        const MaiFilePath parent = path.dirName();
        if (!parent.isEmpty() && parent != path) {
            const MaiError error = MaiFileSystem::createDirectories(parent);
            if (error.hasError())
                return MaiToolResult::failure(
                    error.code(), "could not create parent directory: " + error.message());
        }
        const MaiError error = MaiFileSystem::createEmptyFile(path);
        if (error.hasError()) {
            if (MaiFileSystem::exists(path))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "path already exists; nothing was overwritten");
            return MaiToolResult::failure(error.code(), error.message());
        }
        return MaiToolResult::success("Created file " + toRelativePath(context.root, path));
    }
};

class CreateDirectoryTool final : public MaiTool {
public:
    std::string name() const override {
        return "create_directory";
    }
    std::string description() const override {
        return "Create a directory and any missing parents. An existing directory is accepted; "
               "a file at that path is an error.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string","description":"Absolute path or path relative to the working directory"}},"required":["path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const std::string path =
            context.resolvePath(stringArgument(parseArguments(argumentsJson), "path"));
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"directory:" + path};
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        if (args.size() != 1 || !args.contains("path") || !args["path"].is_string())
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "create_directory requires one string parameter named path");
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return resolved.error;
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved.path);
        if (MaiFileSystem::exists(path) && !MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "a file already exists at the requested directory path");
        const bool existed = MaiFileSystem::isDirectory(path);
        const MaiError error = MaiFileSystem::createDirectories(path);
        if (error.hasError()) return MaiToolResult::failure(error.code(), error.message());
        if (!MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "directory creation returned without a directory");
        return MaiToolResult::success(
            std::string(existed ? "Directory already exists " : "Created directory ") +
            toRelativePath(context.root, path));
    }
};

class DeleteFileTool final : public MaiTool {
public:
    std::string name() const override {
        return "delete_file";
    }
    std::string description() const override {
        return "Delete one existing file. Directories are never deleted; this is not recursive.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string","description":"Absolute path or path relative to the working directory"}},"required":["path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const std::string path =
            context.resolvePath(stringArgument(parseArguments(argumentsJson), "path"));
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"file:" + path};
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        if (args.size() != 1 || !args.contains("path") || !args["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "delete_file requires one string parameter named path");
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return resolved.error;
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved.path);
        MaiFilePath requested = MaiFilePath::fromUtf8(stringArgument(args, "path"));
        if (!requested.isAbsolute())
            requested = MaiFilePath::fromUtf8(context.root).append(requested);
        if (MaiFileSystem::isSymbolicLink(requested))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "delete_file refuses symbolic links; use shell with "
                                          "explicit approval to remove the link itself");
        if (!MaiFileSystem::exists(path))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "file does not exist: " + stringArgument(args, "path"));
        if (MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "delete_file cannot delete a directory");
        const MaiError error = MaiFileSystem::removeFile(path);
        if (error.hasError()) return MaiToolResult::failure(error.code(), error.message());
        return MaiToolResult::success("Deleted file " + toRelativePath(context.root, path));
    }
};

// ── write ───────────────────────────────────────────────────────
class WriteTool final : public MaiTool {
public:
    std::string name() const override {
        return "write";
    }
    std::string description() const override {
        return "Write content to a file, replacing whatever was "
               "there. Parent directories are created as needed.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"Absolute path or path relative to the working directory"},)"
               R"("content":{"type":"string","description":"The full content to write"}},)"
               R"("required":["path","content"]})";
    }
    // 会改文件。M4 的闸门就位后这里会真正拦一道。
    bool requiresApproval(const std::string& argumentsJson) const override {
        (void)argumentsJson;
        return true;
    }

    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const json args = parseArguments(argumentsJson);
        if (!args.contains("path") || !args["path"].is_string())
            return MaiTool::approvalKeys(argumentsJson, context);
        const std::string path = context.resolvePath(args["path"].get<std::string>());
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"file:" + path};
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
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
        return "Find files by name pattern. Relative paths use the working directory. Supports *, "
               "?, ** to "
               "cross directories, and brace lists such as *.{png,jpg}. For a broad search, "
               "provide a subdirectory in path.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("pattern":{"type":"string","description":"For example **/*.cpp or src/*.h"},)"
               R"("path":{"type":"string","description":"Subdirectory to search from, defaults to the working directory root"}},)"
               R"("required":["pattern"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string pattern = stringArgument(args, "pattern");
        if (pattern.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: pattern");
        std::vector<std::string> patterns;
        if (!expandGlobBraces(pattern, patterns))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "invalid or overly broad glob brace pattern");

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
        if (basePath == basePath.dirName() && pattern.find("**") != std::string::npos)
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "Searching an entire filesystem root is too broad. Set path to a specific "
                "directory, such as the user's Documents folder, and retry.");

        std::vector<std::string> hits;
        int visitedEntries = 0;
        bool budgetExceeded = false;
        const auto deadline = std::chrono::steady_clock::now() + kMaxGlobDuration;
        MaiFileSystem::walk(basePath, [&](const MaiFileEntry& entry) {
            if (context.isCanceled()) return MaiWalkAction::Stop;
            ++visitedEntries;
            if (visitedEntries > kMaxGlobVisitedEntries ||
                (visitedEntries % 128 == 0 && std::chrono::steady_clock::now() >= deadline)) {
                budgetExceeded = true;
                return MaiWalkAction::Stop;
            }
            if (entry.isDirectory) {
                return shouldSkipDirectory(entry.nameUtf8) ? MaiWalkAction::SkipDirectory
                                                           : MaiWalkAction::Continue;
            }
            const std::string relative = toRelativePath(context.root, entry.path);
            const bool matches =
                std::any_of(patterns.begin(), patterns.end(), [&](const auto& item) {
                    return globMatch(item, relative) || globMatch(item, entry.nameUtf8);
                });
            if (matches) {
                hits.push_back(relative);
                if (static_cast<int>(hits.size()) >= kMaxGlobResults) return MaiWalkAction::Stop;
            }
            return MaiWalkAction::Continue;
        });

        if (budgetExceeded && hits.empty())
            return MaiToolResult::success(
                "Search stopped after the scan limit without finding a match. Set path to a "
                "smaller directory and retry.",
                true);
        if (hits.empty()) return MaiToolResult::success("No files match " + pattern);
        std::sort(hits.begin(), hits.end());
        std::string out;
        for (const auto& hit : hits) {
            out += hit;
            out += "\n";
        }
        const bool truncated = budgetExceeded || static_cast<int>(hits.size()) >= kMaxGlobResults;
        if (truncated)
            out +=
                "...Search was limited. Set path to a smaller directory or use a more "
                "specific pattern to find other matches.";
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
        return "Search text files with a regular expression. Source bytes are converted to "
               "UTF-8 before matching; set encoding for ambiguous legacy text. Relative paths "
               "use the working directory. Returns the file, line number and matching line.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("pattern":{"type":"string","description":"Regular expression"},)"
               R"("path":{"type":"string","description":"Subdirectory to search from, defaults to the working directory root"},)"
               R"("encoding":{"type":"string","description":"Source text encoding; default auto"},)"
               R"("glob":{"type":"string","description":"Only search files matching this pattern, for example *.cpp"}},)"
               R"("required":["pattern"]})";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        if (args.contains("encoding") && !args["encoding"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "encoding must be a string");
        const std::string pattern = stringArgument(args, "pattern");
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

        const std::string globPattern = stringArgument(args, "glob");
        const bool hasFilter = !globPattern.empty();

        std::string out;
        int matches = 0;
        int undecodable = 0;

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

            // 太大的文件跳过：多半是二进制或产物，搜了也没意义还很慢。大小是遍历时顺路拿到的，
            // 不用再 stat 一次。
            if (entry.size > 2u * 1024 * 1024) return MaiWalkAction::Continue;

            std::string blob;
            if (MaiFileSystem::readFile(entry.path, blob).hasError())
                return MaiWalkAction::Continue;
            const MaiResult<std::string> decoded =
                decodeFileText(blob, stringArgument(args, "encoding"), context);
            if (!decoded) {
                ++undecodable;
                return MaiWalkAction::Continue;
            }
            blob = decoded.value();

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
                // 先截断再匹配。用户给的正则可能有灾难性回溯，
                // 行越长越容易把栈打穿——glob 那边已经因此崩过一次，这里不重蹈覆辙。
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

        if (matches == 0) {
            const std::string message = "No content matches " + pattern;
            if (undecodable == 0) return MaiToolResult::success(message);
            return MaiToolResult::success(
                message + "; skipped " + std::to_string(undecodable) +
                    " files that could not be decoded. Retry with an explicit encoding.",
                true);
        }
        const bool truncated =
            matches >= kMaxGrepMatches || out.size() > kMaxOutputBytes || undecodable > 0;
        truncateUtf8(out, kMaxOutputBytes);
        if (undecodable > 0)
            out += "\n...Skipped " + std::to_string(undecodable) +
                   " files that could not be decoded; specify encoding to search them.";
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
std::unique_ptr<MaiTool> makeMaiCreateFileTool() {
    return std::make_unique<CreateFileTool>();
}
std::unique_ptr<MaiTool> makeMaiCreateDirectoryTool() {
    return std::make_unique<CreateDirectoryTool>();
}
std::unique_ptr<MaiTool> makeMaiDeleteFileTool() {
    return std::make_unique<DeleteFileTool>();
}
std::unique_ptr<MaiTool> makeMaiGlobTool() {
    return std::make_unique<GlobTool>();
}
std::unique_ptr<MaiTool> makeMaiGrepTool() {
    return std::make_unique<GrepTool>();
}
