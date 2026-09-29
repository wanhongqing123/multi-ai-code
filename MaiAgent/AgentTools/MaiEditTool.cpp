#include <json.hpp>

#include <cstddef>
#include <string>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiEditTool.h"

// edit：把文件里的一段原文换成另一段。
//
// ── 为什么不能只有 write ────────────────────────────────────────
//
// write 是整份覆写。要改一行，模型得把整个文件重新吐一遍：
//
//   慢     一个 800 行的文件要吐 800 行，而真正变的只有一行
//   贵     那 800 行按输出计费，每改一次付一次
//   危险   吐的过程中任何一处走样都会**悄悄**改掉别的地方——
//          模型不是复制粘贴，它是重新生成，中间漏一行没有任何报错
//
// 所以要有一个「只说改动」的工具。这是 codex 的 apply_patch 和 opencode 的 edit
// 解决的同一个问题。
//
// ── 为什么要求 old_string 唯一 ──────────────────────────────────
//
// 不唯一就没法知道改的是哪一处。默认只替换一处、并且要求全文只出现一次，
// 匹配到多处就失败并告诉模型「多带几行上下文再来」。
// 真要全改的，显式传 replace_all。

namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;
// 改完回给模型看的那一小段上下文。给全文没意义（它刚给过），一点不给又看不出改对没有。
constexpr std::size_t kContextChars = 160;

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t total = 0;
    std::size_t at = haystack.find(needle);
    while (at != std::string::npos) {
        ++total;
        at = haystack.find(needle, at + needle.size());
    }
    return total;
}

// 取改动处前后的一小段，让模型确认改在了它想改的地方。
std::string excerptAround(const std::string& text, std::size_t at, std::size_t length) {
    const std::size_t begin = at > kContextChars ? at - kContextChars : 0;
    const std::size_t end = std::min(text.size(), at + length + kContextChars);
    std::string piece = text.substr(begin, end - begin);
    if (begin > 0) piece.insert(0, "...");
    if (end < text.size()) piece += "...";
    return piece;
}

class EditTool final : public MaiTool {
public:
    std::string name() const override {
        return "edit";
    }

    std::string description() const override {
        return "Replace an exact piece of text in a file. Prefer this over write when you are "
               "changing part of an existing file: it only sends the difference, so it is faster "
               "and cannot silently rewrite the parts you did not mean to touch. old_string must "
               "match the file exactly, including indentation, and must appear only once unless "
               "replace_all is set.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"File path relative to the working directory"},)"
               R"("old_string":{"type":"string","description":"The exact text to replace, copied from the file"},)"
               R"("new_string":{"type":"string","description":"What to put there instead"},)"
               R"("replace_all":{"type":"boolean","description":"Replace every occurrence instead of requiring exactly one, default false"}},)"
               R"("required":["path","old_string","new_string"]})";
    }

    // 改文件，一律要问。危险程度不随参数变，所以不用看参数。
    bool requiresApproval(const std::string& argumentsJson) const override {
        (void)argumentsJson;
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string requested = args.value("path", std::string{});
        if (requested.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: path");
        }
        if (context.root.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "this session has no working directory, so file tools are unavailable");
        }
        const std::string resolved = maiResolvePathWithinRoot(context.root, requested);
        if (resolved.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "path is outside the working directory, which is not allowed: " + requested);
        }
        if (!args.contains("old_string") || !args["old_string"].is_string() ||
            !args.contains("new_string") || !args["new_string"].is_string()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "old_string and new_string are both required and must be strings");
        }

        const std::string oldText = args["old_string"].get<std::string>();
        const std::string newText = args["new_string"].get<std::string>();
        const bool replaceAll = args.value("replace_all", false);

        if (oldText.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "old_string is empty. Use write to create a file or replace all of its contents.");
        }
        if (oldText == newText) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "old_string and new_string are identical, so there is "
                                          "nothing to change");
        }

        const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
        if (!MaiFileSystem::exists(path)) {
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "file does not exist: " + requested);
        }
        if (MaiFileSystem::isDirectory(path)) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "that is a directory, not a file: " + requested);
        }

        std::string contents;
        bool readTruncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(path, contents, kMaxFileBytes, &readTruncated);
        if (readError.hasError()) {
            return MaiToolResult::failure(readError.code(), readError.message());
        }
        if (readTruncated) {
            // 截断过的内容写回去会**把文件尾巴砍掉**。宁可不改。
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "this file is too large to edit safely; it would be truncated");
        }

        const std::size_t matches = countOccurrences(contents, oldText);
        if (matches == 0) {
            // 这句话要写成模型能据此改正的样子。最常见的原因是缩进抄错，或者
            // 它凭记忆写而不是从 read 的结果里抄。
            return MaiToolResult::failure(
                MaiErrorCode::NotFound,
                "old_string was not found in " + requested +
                    ". Copy it exactly from the file, including indentation and line breaks. "
                    "Read the file again if you are not sure.");
        }
        if (matches > 1 && !replaceAll) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "old_string appears " + std::to_string(matches) + " times in " + requested +
                    ". Include a few more surrounding lines so it matches exactly one place, "
                    "or set replace_all to change all of them.");
        }

        const std::size_t firstAt = contents.find(oldText);
        std::string updated;
        if (replaceAll) {
            updated.reserve(contents.size());
            std::size_t cursor = 0;
            while (true) {
                const std::size_t at = contents.find(oldText, cursor);
                if (at == std::string::npos) {
                    updated.append(contents, cursor, std::string::npos);
                    break;
                }
                updated.append(contents, cursor, at - cursor);
                updated += newText;
                cursor = at + oldText.size();
            }
        } else {
            updated = contents;
            updated.replace(firstAt, oldText.size(), newText);
        }

        const MaiError writeError = MaiFileSystem::writeFile(path, updated);
        if (writeError.hasError()) {
            return MaiToolResult::failure(writeError.code(), writeError.message());
        }

        const std::size_t changed = replaceAll ? matches : 1;
        std::string report = "Replaced " + std::to_string(changed) +
                             (changed == 1 ? " occurrence in " : " occurrences in ") + requested +
                             ".\n\n" + excerptAround(updated, firstAt, newText.size());
        return MaiToolResult::success(std::move(report));
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiEditTool() {
    return std::make_unique<EditTool>();
}
