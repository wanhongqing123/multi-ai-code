#include <cstdio>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiScreenshot.h"
#include "MaiScreenshotTool.h"
#include "MaiTool.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

void test_tool_contract() {
    std::unique_ptr<MaiTool> tool = makeMaiScreenshotTool();
    CHECK(tool != nullptr);
    CHECK(tool->name() == "screenshot");
    CHECK(tool->requiresApproval("{}"));
    CHECK(tool->parametersSchema().find("additionalProperties") != std::string::npos);
    CHECK(tool->parametersSchema().find("windowTitle") != std::string::npos);

    MaiToolContext textOnly;
    textOnly.model = "glm-5.3";
    const MaiToolResult unsupported = tool->execute("{}", textOnly);
    CHECK(unsupported.hasError());
    CHECK(unsupported.error().code() == MaiErrorCode::NotSupported);
    CHECK(unsupported.error().message().find("glm-5.3-flash") != std::string::npos);

    MaiToolContext imageCapable;
    imageCapable.model = "glm-5.3-flash";
    const MaiToolResult invalid = tool->execute("not-json", imageCapable);
    CHECK(invalid.hasError());
    CHECK(invalid.error().code() == MaiErrorCode::InvalidInput);
    const MaiToolResult wrongModeType = tool->execute(R"({"mode":7})", imageCapable);
    CHECK(wrongModeType.hasError());
    CHECK(wrongModeType.error().code() == MaiErrorCode::InvalidInput);
    const MaiToolResult extraArgument = tool->execute(R"({"unexpected":true})", imageCapable);
    CHECK(extraArgument.hasError());
    CHECK(extraArgument.error().code() == MaiErrorCode::InvalidInput);
}

void test_builtin_registration_matches_platform_support() {
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    CHECK((registry.find("screenshot") != nullptr) == maiIsScreenshotSupported());
}

void checkPngAndRemove(const MaiToolResult& captured) {
    CHECK(!captured.hasError());
    CHECK(captured.images().size() == 1);
    if (captured.hasError() || captured.images().empty()) return;

    const MaiToolImage& image = captured.images().front();
    CHECK(image.mimeType == "image/png");
    const MaiFilePath path = MaiFilePath::fromUtf8(image.path);
    CHECK(path.isAbsolute());

    std::string bytes;
    const MaiError readError = MaiFileSystem::readFile(path, bytes, 16);
    CHECK(!readError.hasError());
    const std::string signature("\x89PNG\r\n\x1a\n", 8);
    CHECK(bytes.size() >= signature.size());
    CHECK(bytes.compare(0, signature.size(), signature) == 0);
    CHECK(!MaiFileSystem::removeFile(path).hasError());
}

void test_native_capture_returns_a_png() {
    if (!maiIsScreenshotSupported()) return;

    std::unique_ptr<MaiTool> tool = makeMaiScreenshotTool();
    MaiToolContext context;
    context.model = "glm-5.3-flash";
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    const MaiToolResult captured = tool->execute("{}", context);
    if (captured.hasError()) std::printf("capture error: %s\n", captured.error().message().c_str());
    checkPngAndRemove(captured);
}

void test_native_window_capture_returns_a_png() {
#if defined(_WIN32)
    constexpr wchar_t kTitle[] = L"MaiAgent Native Window Capture Test";
    const HWND window =
        ::CreateWindowExW(0, L"STATIC", kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640,
                          360, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    CHECK(window != nullptr);
    if (window == nullptr) return;
    ::ShowWindow(window, SW_SHOW);
    ::UpdateWindow(window);

    std::unique_ptr<MaiTool> tool = makeMaiScreenshotTool();
    MaiToolContext context;
    context.model = "glm-5.3-flash";
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    const MaiToolResult captured = tool->execute(
        R"({"mode":"window","windowTitle":"MaiAgent Native Window Capture Test"})", context);
    if (captured.hasError())
        std::printf("window capture error: %s\n", captured.error().message().c_str());
    checkPngAndRemove(captured);
    ::DestroyWindow(window);
#endif
}

}  // namespace

int main() {
    test_tool_contract();
    test_builtin_registration_matches_platform_support();
    test_native_capture_returns_a_png();
    test_native_window_capture_returns_a_png();
    if (failures == 0) std::printf("screenshot tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
