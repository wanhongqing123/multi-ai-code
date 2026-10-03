#include "MaiAgentSendMediaTool.h"

#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int main() {
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-agent-send-media-tests"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath image = root.append(MaiFilePath::fromUtf8("result.png"));
    const std::string png =
        "\x89PNG\r\n\x1a\n"
        "test-image-bytes";
    CHECK(!MaiFileSystem::writeFile(image, png));
    MaiToolContext context;
    context.root = root.toUtf8();
    auto tool = makeMaiAgentSendMediaTool();
    CHECK(tool && tool->name() == "agent_send_media");
    const MaiToolResult delivered = tool->execute(
        R"({"file_path":"result.png","type":"image","caption":"Edited photo"})", context);
    CHECK(!delivered.hasError());
    const auto output = nlohmann::json::parse(delivered.output());
    CHECK(output["delivery"] == "current_ai_session");
    CHECK(output["path"] == "result.png");
    CHECK(output["mime_type"] == "image/png");
    CHECK(output["caption"] == "Edited photo");
    CHECK(MaiFileSystem::exists(image));
    CHECK(tool->execute(R"({"file_path":"../result.png","type":"image"})", context).hasError());
    CHECK(tool->execute(R"({"file_path":"result.png","type":"video"})", context).hasError());
    CHECK(tool->execute(R"({"file_path":"result.png","type":"image","peer_id":"someone"})", context)
              .hasError());
    MaiFileSystem::removeRecursively(root);
    return 0;
}
