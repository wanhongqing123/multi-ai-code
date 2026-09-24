#include "MaiScreenshotTool.h"

#include <memory>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiScreenshot.h"

using json = nlohmann::json;

namespace {

class MaiScreenshotTool final : public MaiTool {
public:
    std::string name() const override {
        return "screenshot";
    }

    std::string description() const override {
        return "Capture a display or a visible top-level window and inspect the returned image. "
               "Use mode=display for the display containing the mouse pointer. Use mode=window "
               "with windowTitle when the user names a specific window.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("mode":{"type":"string","enum":["display","window"],"default":"display"},)"
               R"("windowTitle":{"type":"string","description":"Visible window title; required for window mode"}},)"
               R"("required":[],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const json arguments = json::parse(argumentsJson, nullptr, /*allow_exceptions=*/false);
        if (arguments.is_discarded() || !arguments.is_object()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot expects a JSON object");
        }
        for (auto field = arguments.begin(); field != arguments.end(); ++field) {
            if (field.key() != "mode" && field.key() != "windowTitle") {
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "screenshot received an unknown parameter: " + field.key());
            }
        }
        if (arguments.contains("mode") && !arguments["mode"].is_string()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot mode must be a string");
        }
        const std::string mode = arguments.contains("mode") ? arguments["mode"].get<std::string>()
                                                            : std::string("display");
        MaiScreenshotRequest request;
        if (mode == "window") {
            request.target = MaiScreenshotTarget::WindowByTitle;
            if (!arguments.contains("windowTitle") || !arguments["windowTitle"].is_string() ||
                arguments["windowTitle"].get_ref<const std::string&>().empty()) {
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "windowTitle is required and must be non-empty when mode is window");
            }
            request.windowTitle = arguments["windowTitle"].get<std::string>();
        } else if (mode != "display") {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "screenshot mode must be display or window");
        }
        if (context.isCanceled()) {
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
        }
        if (context.model == "glm-5.3") {
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "The current glm-5.3 model accepts text only. Ask the user to switch to "
                "glm-5.3-flash before taking a screenshot.");
        }
        if (!maiIsScreenshotSupported()) {
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "native screen capture is not supported on this platform");
        }

        MaiScreenshot screenshot;
        const MaiError captureError = maiCaptureScreenshot(request, screenshot);
        if (captureError.hasError()) {
            return MaiToolResult::failure(captureError.code(), captureError.message());
        }
        if (screenshot.pngBytes.empty() || screenshot.width <= 0 || screenshot.height <= 0) {
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "native screen capture returned an empty image");
        }

        const MaiFilePath directory = MaiFileSystem::temporaryDirectory()
                                          .append(MaiFilePath::fromUtf8("MaiAgent"))
                                          .append(MaiFilePath::fromUtf8("screenshots"));
        const MaiError directoryError = MaiFileSystem::createDirectories(directory);
        if (directoryError.hasError()) {
            return MaiToolResult::failure(directoryError.code(), directoryError.message());
        }

        const std::string fileName = MaiIdGenerator::generate("screenshot_") + ".png";
        const MaiFilePath path = directory.append(MaiFilePath::fromUtf8(fileName));
        const std::string bytes(reinterpret_cast<const char*>(screenshot.pngBytes.data()),
                                screenshot.pngBytes.size());
        const MaiError writeError = MaiFileSystem::writeFile(path, bytes);
        if (writeError.hasError()) {
            return MaiToolResult::failure(writeError.code(), writeError.message());
        }

        const std::string target = request.target == MaiScreenshotTarget::WindowByTitle
                                       ? "the requested window"
                                       : "the current display";
        const std::string output = "Captured " + target + " at " +
                                   std::to_string(screenshot.width) + "x" +
                                   std::to_string(screenshot.height) + ".";
        return MaiToolResult::successWithImages(output, {MaiToolImage{path.toUtf8(), "image/png"}});
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiScreenshotTool() {
    return std::make_unique<MaiScreenshotTool>();
}
