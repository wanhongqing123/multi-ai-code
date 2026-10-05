#include "MaiZlibTool.h"

#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

namespace {

int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

}  // namespace

int main() {
    const std::string text = "hello hello hello\n" + std::string(1024, 'x');
    for (const std::string format : {"gzip", "zlib", "deflate"}) {
        const auto packed = maiTransformZlibBytes(text, true, format, 4096);
        CHECK(packed);
        if (!packed) continue;
        const auto unpacked = maiTransformZlibBytes(packed.value(), false,
                                                    format == "deflate" ? "deflate" : "auto", 4096);
        CHECK(unpacked);
        if (unpacked) CHECK(unpacked.value() == text);
        CHECK(!maiTransformZlibBytes(packed.value(), false,
                                     format == "deflate" ? "deflate" : "auto", 32));
        CHECK(!maiTransformZlibBytes(packed.value() + "junk", false,
                                     format == "deflate" ? "deflate" : "auto", 4096));
    }
    CHECK(!maiTransformZlibBytes("broken", false, "auto", 4096));

    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-zlib-" + MaiIdGenerator::newPartId()));
    CHECK(!MaiFileSystem::createDirectories(root).hasError());
    const MaiFilePath input = root.append(MaiFilePath::fromUtf8("message.txt"));
    CHECK(!MaiFileSystem::writeFile(input, text).hasError());
    MaiToolContext context;
    context.root = root.toUtf8();
    auto compress = makeMaiZlibCompressTool();
    auto decompress = makeMaiZlibDecompressTool();
    CHECK(compress->requiresApproval("{}"));
    CHECK(compress->execute(R"({"input_path":"../escape"})", context).hasError());
    const auto result = compress->execute(R"({"input_path":"message.txt"})", context);
    CHECK(!result.hasError());
    if (!result.hasError()) {
        const auto output = nlohmann::json::parse(result.output());
        CHECK(output.value("output_path", std::string{}) == "message.txt.gz");
        CHECK(MaiFileSystem::exists(root.append(MaiFilePath::fromUtf8("message.txt.gz"))));
        CHECK(compress->execute(R"({"input_path":"message.txt"})", context).hasError());
        const auto restored = decompress->execute(
            R"({"input_path":"message.txt.gz","output_path":"restored.txt"})", context);
        CHECK(!restored.hasError());
        std::string contents;
        CHECK(!MaiFileSystem::readFile(root.append(MaiFilePath::fromUtf8("restored.txt")), contents)
                   .hasError());
        CHECK(contents == text);
    }
    MaiFileSystem::removeRecursively(root);
    return failures ? 1 : 0;
}
