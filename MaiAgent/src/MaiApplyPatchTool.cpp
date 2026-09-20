#include <json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiTool.h"

// apply_patch：一次提交多处改动。
//
// ── 和 edit 的分工 ──────────────────────────────────────────────
//
// edit 一次换一处。改五个地方就要调五次，每次都要用户点一次头，而且中间任何一次
// 失败都会留下**改了一半**的文件——模型和用户都说不清当前是什么状态。
//
// apply_patch 把整批改动当一个事务：**全部能应用才落盘，任何一处对不上就整批不动**。
// 跨文件的改动（改接口同时改所有调用方）只有这个能做对。
//
// ── 格式 ────────────────────────────────────────────────────────
//
// 照抄 codex 的 apply-patch，不自己发明：
//
//   *** Begin Patch
//   *** Update File: src/a.cpp
//   @@
//    context line
//   -removed
//   +added
//   @@
//    more context
//   -old
//   +new
//   *** Add File: src/b.cpp
//   +first line
//   +second line
//   *** Delete File: src/c.cpp
//   *** Update File: src/d.cpp
//   *** Move to: src/e.cpp
//   @@
//   -x
//   +y
//   *** End Patch
//
// 行首那个字符是有意义的：空格 = 保持不变的上下文，`-` = 删掉，`+` = 加上。
// 上下文行**必须带那个前导空格**，不带的话没法和 `@@` 之类的标记区分开。

namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

// 一段连续的改动。
struct Chunk {
    // 要在文件里找到的那一段：上下文行 + 被删掉的行，按原顺序。
    std::vector<std::string> pattern;
    // 换成什么：上下文行 + 新加的行。
    std::vector<std::string> replacement;
    // 这一段标了 *** End of File，应该贴着文件末尾找。
    bool anchoredAtEnd = false;
};

struct Operation {
    enum class Kind { Add, Delete, Update };
    Kind kind = Kind::Update;
    std::string path;      // 模型给的相对路径，报错时要原样回显
    std::string movePath;  // Update 带 *** Move to 时的新路径
    std::string addContent;
    std::vector<Chunk> chunks;
};

bool startsWith(const std::string& text, const char* prefix) {
    const std::size_t length = std::char_traits<char>::length(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

std::string afterPrefix(const std::string& text, const char* prefix) {
    std::string rest = text.substr(std::char_traits<char>::length(prefix));
    const std::size_t begin = rest.find_first_not_of(" \t");
    if (begin == std::string::npos) return std::string();
    const std::size_t end = rest.find_last_not_of(" \t\r");
    return rest.substr(begin, end - begin + 1);
}

// 按 \n 切行，把每行末尾的 \r 摘掉，同时记下这个文件本来用的是哪种换行。
//
// **换行必须原样还回去。** 把一个 CRLF 的文件写成 LF，git 会把整个文件报成改动过，
// 真正的那几行改动就淹没了——这是 Windows 上最容易踩的一脚。
std::vector<std::string> splitLines(const std::string& text, std::string& ending,
                                    bool& endsWithNewline) {
    ending = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    endsWithNewline = !text.empty() && text.back() == '\n';
    std::vector<std::string> lines;
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t newlineAt = text.find('\n', cursor);
        if (newlineAt == std::string::npos) {
            if (cursor < text.size()) lines.push_back(text.substr(cursor));
            break;
        }
        std::string line = text.substr(cursor, newlineAt - cursor);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        cursor = newlineAt + 1;
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines, const std::string& ending,
                      bool endsWithNewline) {
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text += lines[i];
        if (i + 1 < lines.size() || endsWithNewline) text += ending;
    }
    return text;
}

std::string trimEnd(const std::string& text) {
    const std::size_t end = text.find_last_not_of(" \t");
    return end == std::string::npos ? std::string() : text.substr(0, end + 1);
}

std::string trimBoth(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return std::string();
    const std::size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

// 在 lines 里从 start 开始找 pattern，返回起始下标，找不到返回 npos。
//
// 严格程度**逐级放松**：先精确比，再忽略行尾空白，最后两头空白都忽略。
// 模型抄上下文时最常见的偏差就是缩进和行尾空格，一次都不让步的话
// 大部分补丁都会失败，而失败的理由（"少了一个空格"）它自己看不出来。
// 这三级和 codex 的 seek_sequence 一致。
std::size_t findSequence(const std::vector<std::string>& lines,
                         const std::vector<std::string>& pattern, std::size_t start,
                         bool anchoredAtEnd) {
    if (pattern.empty()) return start;
    if (pattern.size() > lines.size()) return std::string::npos;

    const std::size_t limit = lines.size() - pattern.size();
    // 标了 End of File 的先贴着末尾试一次：模型想往文件尾巴上加东西时，
    // 它给的那点上下文常常在文件中间也能匹配上。
    if (anchoredAtEnd && limit >= start) {
        bool same = true;
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if (lines[limit + i] != pattern[i]) {
                same = false;
                break;
            }
        }
        if (same) return limit;
    }

    for (int strictness = 0; strictness < 3; ++strictness) {
        for (std::size_t at = start; at <= limit; ++at) {
            bool same = true;
            for (std::size_t i = 0; i < pattern.size(); ++i) {
                const std::string& have = lines[at + i];
                const std::string& want = pattern[i];
                const bool equal = strictness == 0   ? have == want
                                   : strictness == 1 ? trimEnd(have) == trimEnd(want)
                                                     : trimBoth(have) == trimBoth(want);
                if (!equal) {
                    same = false;
                    break;
                }
            }
            if (same) return at;
        }
    }
    return std::string::npos;
}

struct ParseOutcome {
    bool ok = false;
    std::string message;
    std::vector<Operation> operations;
};

ParseOutcome parsePatch(const std::string& patch) {
    ParseOutcome outcome;
    std::string ending;
    bool endsWithNewline = false;
    const std::vector<std::string> lines = splitLines(patch, ending, endsWithNewline);

    std::size_t at = 0;
    while (at < lines.size() && trimBoth(lines[at]).empty()) ++at;
    if (at >= lines.size() || trimBoth(lines[at]) != "*** Begin Patch") {
        outcome.message = "the patch must start with \"*** Begin Patch\"";
        return outcome;
    }
    ++at;

    Operation current;
    bool hasCurrent = false;
    Chunk chunk;
    bool hasChunk = false;
    bool sawEnd = false;

    const auto closeChunk = [&]() {
        if (hasChunk && (!chunk.pattern.empty() || !chunk.replacement.empty())) {
            current.chunks.push_back(chunk);
        }
        chunk = Chunk{};
        hasChunk = false;
    };
    const auto closeOperation = [&]() {
        closeChunk();
        if (hasCurrent) outcome.operations.push_back(current);
        current = Operation{};
        hasCurrent = false;
    };

    for (; at < lines.size(); ++at) {
        const std::string& line = lines[at];
        const std::string trimmed = trimBoth(line);

        if (trimmed == "*** End Patch") {
            sawEnd = true;
            break;
        }
        if (startsWith(trimmed, "*** Update File:")) {
            closeOperation();
            current.kind = Operation::Kind::Update;
            current.path = afterPrefix(trimmed, "*** Update File:");
            hasCurrent = true;
            continue;
        }
        if (startsWith(trimmed, "*** Add File:")) {
            closeOperation();
            current.kind = Operation::Kind::Add;
            current.path = afterPrefix(trimmed, "*** Add File:");
            hasCurrent = true;
            continue;
        }
        if (startsWith(trimmed, "*** Delete File:")) {
            closeOperation();
            current.kind = Operation::Kind::Delete;
            current.path = afterPrefix(trimmed, "*** Delete File:");
            hasCurrent = true;
            continue;
        }
        if (startsWith(trimmed, "*** Move to:")) {
            if (!hasCurrent || current.kind != Operation::Kind::Update) {
                outcome.message = "\"*** Move to:\" must follow an \"*** Update File:\" line";
                return outcome;
            }
            current.movePath = afterPrefix(trimmed, "*** Move to:");
            continue;
        }
        if (trimmed == "*** End of File") {
            chunk.anchoredAtEnd = true;
            continue;
        }
        if (!hasCurrent) {
            if (trimmed.empty()) continue;
            outcome.message =
                "every change must come after an \"*** Add File:\", \"*** Update File:\" or "
                "\"*** Delete File:\" line";
            return outcome;
        }

        if (current.kind == Operation::Kind::Add) {
            // 新建文件的每一行都要带 `+`，和 Update 里的加行保持一致。
            if (!line.empty() && line[0] == '+') {
                current.addContent += line.substr(1);
                current.addContent += "\n";
            } else if (trimmed.empty()) {
                current.addContent += "\n";
            } else {
                outcome.message = "lines in \"*** Add File:\" must start with \"+\"";
                return outcome;
            }
            continue;
        }
        if (current.kind == Operation::Kind::Delete) {
            if (trimmed.empty()) continue;
            outcome.message = "\"*** Delete File:\" does not take any lines";
            return outcome;
        }

        if (startsWith(line, "@@")) {
            closeChunk();
            hasChunk = true;
            continue;
        }
        if (line.empty()) {
            // 补丁里的空行代表原文里的空上下文行。
            if (!hasChunk) hasChunk = true;
            chunk.pattern.push_back(std::string());
            chunk.replacement.push_back(std::string());
            continue;
        }
        if (!hasChunk) hasChunk = true;
        const char marker = line[0];
        const std::string body = line.substr(1);
        if (marker == ' ') {
            chunk.pattern.push_back(body);
            chunk.replacement.push_back(body);
        } else if (marker == '-') {
            chunk.pattern.push_back(body);
        } else if (marker == '+') {
            chunk.replacement.push_back(body);
        } else {
            outcome.message = "every change line must start with a space, \"-\" or \"+\", but got: " +
                              line;
            return outcome;
        }
    }

    if (!sawEnd) {
        outcome.message = "the patch must end with \"*** End Patch\"";
        return outcome;
    }
    closeOperation();
    if (outcome.operations.empty()) {
        outcome.message = "the patch does not contain any file changes";
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

// 一次落盘计划。**先全部算完再写**，算的过程中任何一处失败就整批不动。
struct PlannedWrite {
    std::string resolvedPath;
    std::string contents;
    bool remove = false;
    char status = 'M';  // 报给模型看：A 新增 / M 修改 / D 删除
    std::string reportedPath;
};

class ApplyPatchTool final : public MaiTool {
public:
    std::string name() const override {
        return "apply_patch";
    }

    std::string description() const override {
        return "Apply a patch that changes several places, or several files, at once. Use it "
               "instead of repeated edit calls when one change spans multiple files, such as "
               "renaming something and updating every caller. Either the whole patch applies or "
               "nothing does. Format:\n"
               "*** Begin Patch\n"
               "*** Update File: path\n"
               "@@\n"
               " unchanged context line\n"
               "-line to remove\n"
               "+line to add\n"
               "*** Add File: path\n"
               "+new file content\n"
               "*** Delete File: path\n"
               "*** End Patch\n"
               "Context lines must start with a single space and must be copied exactly from the "
               "file.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("patch":{"type":"string","description":"The whole patch, from *** Begin Patch to *** End Patch"}},)"
               R"("required":["patch"]})";
    }

    // 改文件，一律要问。危险程度不随参数变。
    bool requiresApproval(const std::string& argumentsJson) const override {
        (void)argumentsJson;
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string patch = args.value("patch", std::string{});
        if (patch.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: patch");
        }
        if (context.root.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "this session has no working directory, so file tools are unavailable");
        }

        const ParseOutcome parsed = parsePatch(patch);
        if (!parsed.ok) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, parsed.message);
        }

        std::vector<PlannedWrite> plan;
        for (const Operation& operation : parsed.operations) {
            const MaiToolResult failure = planOne(operation, context, plan);
            if (failure.hasError()) return failure;
        }

        // 到这儿为止一个字节都没落盘。下面才真正写。
        //
        // 写的过程还是可能失败（磁盘满、只读文件），那种情况没法完全回滚——
        // 要做到那一步得先把每个文件备份一份。现在的做法是**把能提前查出来的
        // 全在上面查掉**，所以走到这儿失败的概率已经很低了。
        std::string report;
        for (const PlannedWrite& write : plan) {
            const MaiFilePath path = MaiFilePath::fromUtf8(write.resolvedPath);
            if (write.remove) {
                const MaiError error = MaiFileSystem::removeFile(path);
                if (error.hasError()) {
                    return MaiToolResult::failure(error.code(), "could not delete " +
                                                                    write.reportedPath + ": " +
                                                                    error.message());
                }
            } else {
                const MaiFilePath parent = path.dirName();
                if (!parent.isEmpty() && parent != path) {
                    const MaiError error = MaiFileSystem::createDirectories(parent);
                    if (error.hasError()) {
                        return MaiToolResult::failure(
                            error.code(),
                            "could not create parent directory for " + write.reportedPath + ": " +
                                error.message());
                    }
                }
                const MaiError error = MaiFileSystem::writeFile(path, write.contents);
                if (error.hasError()) {
                    return MaiToolResult::failure(error.code(), "could not write " +
                                                                    write.reportedPath + ": " +
                                                                    error.message());
                }
            }
            report += write.status;
            report += ' ';
            report += write.reportedPath;
            report += '\n';
        }
        return MaiToolResult::success("Applied the patch:\n" + report);
    }

private:
    // 把一个操作算成一次落盘计划。失败时返回带错误的结果，成功时返回空结果。
    MaiToolResult planOne(const Operation& operation, const MaiToolContext& context,
                          std::vector<PlannedWrite>& plan) {
        if (operation.path.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "a file header is missing its path");
        }
        const std::string resolved = maiResolvePathWithinRoot(context.root, operation.path);
        if (resolved.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "path is outside the working directory, which is not allowed: " + operation.path);
        }
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved);

        if (operation.kind == Operation::Kind::Add) {
            if (MaiFileSystem::exists(path)) {
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "\"*** Add File\" but " + operation.path +
                        " already exists. Use \"*** Update File\" to change it.");
            }
            plan.push_back({resolved, operation.addContent, false, 'A', operation.path});
            return {};
        }

        if (!MaiFileSystem::exists(path)) {
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "file does not exist: " + operation.path);
        }
        if (MaiFileSystem::isDirectory(path)) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "that is a directory, not a file: " + operation.path);
        }

        if (operation.kind == Operation::Kind::Delete) {
            plan.push_back({resolved, std::string(), true, 'D', operation.path});
            return {};
        }

        std::string contents;
        bool readTruncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(path, contents, kMaxFileBytes, &readTruncated);
        if (readError.hasError()) {
            return MaiToolResult::failure(readError.code(), readError.message());
        }
        if (readTruncated) {
            // 截断过的内容写回去会把文件尾巴砍掉。宁可不改。
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "this file is too large to patch safely; it would be truncated: " + operation.path);
        }

        std::string ending;
        bool endsWithNewline = false;
        std::vector<std::string> lines = splitLines(contents, ending, endsWithNewline);

        std::size_t searchFrom = 0;
        for (std::size_t index = 0; index < operation.chunks.size(); ++index) {
            const Chunk& chunk = operation.chunks[index];
            const std::size_t at =
                findSequence(lines, chunk.pattern, searchFrom, chunk.anchoredAtEnd);
            if (at == std::string::npos) {
                // 这句话要能让模型自己改对。它最常犯的是凭记忆写上下文，
                // 而不是从 read 的结果里逐字抄。
                return MaiToolResult::failure(
                    MaiErrorCode::NotFound,
                    "could not find the context of change " + std::to_string(index + 1) + " in " +
                        operation.path +
                        ". Copy the context lines exactly from the file, including indentation. "
                        "Read the file again if you are not sure. Nothing was changed.");
            }
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(at),
                        lines.begin() + static_cast<std::ptrdiff_t>(at + chunk.pattern.size()));
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                         chunk.replacement.begin(), chunk.replacement.end());
            searchFrom = at + chunk.replacement.size();
        }

        const std::string updated = joinLines(lines, ending, endsWithNewline);
        if (operation.movePath.empty()) {
            plan.push_back({resolved, updated, false, 'M', operation.path});
            return {};
        }

        const std::string destination = maiResolvePathWithinRoot(context.root, operation.movePath);
        if (destination.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "\"*** Move to\" path is outside the working directory, "
                                          "which is not allowed: " +
                                              operation.movePath);
        }
        // 先写新的再删旧的：顺序反了的话中间失败会把内容弄丢。
        plan.push_back({destination, updated, false, 'A', operation.movePath});
        plan.push_back({resolved, std::string(), true, 'D', operation.path});
        return {};
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiApplyPatchTool() {
    return std::make_unique<ApplyPatchTool>();
}
