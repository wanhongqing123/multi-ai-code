#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "MaiTool.h"
#include "MaiViewImageTool.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

struct Workspace {
    fs::path root = fs::temp_directory_path() / ("mai-view-image-" + std::to_string(std::rand()));
    Workspace() {
        fs::create_directories(root);
    }
    ~Workspace() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void test_png_is_returned_as_a_model_image() {
    Workspace workspace;
    const fs::path path = workspace.root / "diagram.bin";
    std::ofstream(path, std::ios::binary).write("\x89PNG\r\n\x1a\n", 8);

    std::unique_ptr<MaiTool> tool = makeMaiViewImageTool();
    MaiToolContext context;
    context.root = workspace.root.u8string();
    context.model = "glm-5.3-flash";
    const MaiToolResult result = tool->execute(R"({"path":"diagram.bin"})", context);

    CHECK(!result.hasError());
    CHECK(result.images().size() == 1);
    if (!result.images().empty()) {
        CHECK(result.images().front().path == path.u8string());
        CHECK(result.images().front().mimeType == "image/png");
    }
}

void test_rejects_invalid_images_and_escaped_paths() {
    Workspace workspace;
    std::ofstream(workspace.root / "text.txt", std::ios::binary) << "not an image";
    std::unique_ptr<MaiTool> tool = makeMaiViewImageTool();
    MaiToolContext context;
    context.root = workspace.root.u8string();
    context.model = "glm-5.3-flash";

    CHECK(tool->execute(R"({"path":"text.txt"})", context).hasError());
    CHECK(tool->execute(R"({"path":"../outside.png"})", context).hasError());
    CHECK(tool->execute(R"({"path":7})", context).hasError());
    CHECK(tool->execute("not-json", context).hasError());
}

void test_rejects_text_only_models() {
    std::unique_ptr<MaiTool> tool = makeMaiViewImageTool();
    MaiToolContext context;
    context.model = "glm-5.3";
    const MaiToolResult result = tool->execute(R"({"path":"image.png"})", context);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::NotSupported);
    CHECK(result.error().message().find("glm-5.3-flash") != std::string::npos);
}

}  // namespace

int main() {
    test_png_is_returned_as_a_model_image();
    test_rejects_invalid_images_and_escaped_paths();
    test_rejects_text_only_models();
    if (failures == 0) std::printf("view image tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
