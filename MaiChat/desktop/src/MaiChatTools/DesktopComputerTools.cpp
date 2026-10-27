#include "MaiChatTools/DesktopComputerTools.h"

#include <QDir>
#include <QSaveFile>
#include <QString>
#include <QUuid>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <mutex>

#include <json.hpp>

#include "MaiChatTools/DesktopComputerPlatform.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiScreenshot.h"
#include "MaiTool.h"

namespace {

using Json = nlohmann::json;

struct ObservedWindow {
    DesktopNativeWindow window;
    std::string token;
    std::chrono::steady_clock::time_point observedAt;
    std::mutex mutex;
};

bool currentWindowMatches(const DesktopNativeWindow& before,
                          const DesktopNativeWindow& now) {
    return before.id == now.id && before.processId == now.processId &&
        before.title == now.title &&
        std::abs(before.x - now.x) < 3 && std::abs(before.y - now.y) < 3 &&
        std::abs(before.width - now.width) < 3 &&
        std::abs(before.height - now.height) < 3;
}

bool ratio(double value) {
    return std::isfinite(value) && value >= 0 && value <= 1;
}

MaiToolResult failure(const MaiError& error) {
    return MaiToolResult::failure(error.code(), error.message());
}

} // namespace

struct DesktopComputerController::Impl {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<ObservedWindow>> sessions;

    std::shared_ptr<ObservedWindow> get(const std::string& sessionId) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = sessions.find(sessionId);
        return found == sessions.end() ? nullptr : found->second;
    }
};

DesktopComputerController::DesktopComputerController() : impl_(std::make_unique<Impl>()) {}
DesktopComputerController::~DesktopComputerController() = default;

MaiResult<DesktopComputerObservation> DesktopComputerController::observe(
    const std::string& sessionId, const std::string& windowTitle,
    const std::string& workspace, bool includeElements) {
    if (sessionId.empty() || windowTitle.empty() || windowTitle.size() > 500 ||
        workspace.empty())
        return {MaiErrorCode::InvalidInput, "Session, exact window title and workspace are required"};
    const auto listed = maiDesktopWindows();
    if (!listed) return listed.error();
    DesktopNativeWindow selected;
    int count = 0;
    for (const auto& window : listed.value()) {
        if (window.title == windowTitle) {
            selected = window;
            ++count;
        }
    }
    if (count != 1)
        return {MaiErrorCode::NotFound,
                count == 0 ? "exact visible window title was not found" :
                             "window title is ambiguous; select a unique title"};

    MaiScreenshot capture;
    MaiScreenshotRequest request;
    request.target = MaiScreenshotTarget::WindowByTitle;
    request.windowTitle = windowTitle;
    const MaiError captured = maiCaptureScreenshot(request, capture);
    if (captured) return captured;
    if (capture.pngBytes.empty() || capture.width <= 0 || capture.height <= 0 ||
        capture.pngBytes.size() > 20 * 1024 * 1024)
        return {MaiErrorCode::Protocol, "selected window screenshot was empty or oversized"};
    const MaiFilePath folder = MaiFilePath::fromUtf8(workspace)
        .append(MaiFilePath::fromUtf8("computer"));
    const MaiError created = MaiFileSystem::createDirectories(folder);
    if (created) return created;
    const MaiFilePath path = folder.append(MaiFilePath::fromUtf8(
        MaiIdGenerator::generate("computer_") + ".png"));
    const std::string nativePath = path.toUtf8();
    QSaveFile file(QString::fromUtf8(nativePath.c_str(), int(nativePath.size())));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char*>(capture.pngBytes.data()),
                   qint64(capture.pngBytes.size())) != qint64(capture.pngBytes.size()) ||
        !file.commit())
        return {MaiErrorCode::Internal, "could not save selected window screenshot"};

    Json output{{"window_id", std::to_string(selected.id)},
                {"title", selected.title}, {"application", selected.application},
                {"process_id", selected.processId},
                {"bounds", {{"x", selected.x}, {"y", selected.y},
                            {"width", selected.width}, {"height", selected.height}}},
                {"image_width", capture.width}, {"image_height", capture.height},
                {"coordinate_space", "relative fractions 0..1 within the captured window"}};
    if (includeElements) {
        const auto elements = maiDesktopElements(selected);
        if (elements) {
            Json items = Json::array();
            for (const auto& item : elements.value()) {
                const double centerX = (item.x + item.width / 2 - selected.x) /
                    selected.width;
                const double centerY = (item.y + item.height / 2 - selected.y) /
                    selected.height;
                if (!ratio(centerX) || !ratio(centerY)) continue;
                const QString label = QString::fromUtf8(item.label.c_str(),
                                                        int(item.label.size())).left(160);
                items.push_back({{"role", item.role},
                    {"label", label.toUtf8().toStdString()},
                    {"center_x_ratio", centerX}, {"center_y_ratio", centerY},
                    {"bounds", {{"x_ratio", (item.x - selected.x) / selected.width},
                                {"y_ratio", (item.y - selected.y) / selected.height},
                                {"width_ratio", item.width / selected.width},
                                {"height_ratio", item.height / selected.height}}}});
            }
            output["elements"] = items;
        } else {
            output["elements"] = Json::array();
            output["element_error"] = elements.error().message();
        }
    }
    auto state = std::make_shared<ObservedWindow>();
    state->window = selected;
    state->token = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    state->observedAt = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->sessions[sessionId] = state;
    }
    output["snapshot_id"] = state->token;
    return DesktopComputerObservation{output.dump(), path.toUtf8()};
}

MaiError DesktopComputerController::act(const std::string& sessionId,
    const std::string& action, const std::string& snapshotId,
    std::uint64_t windowId, double xRatio, double yRatio,
    const std::string& text, const std::string& key, int scrollLines) {
    const auto state = impl_->get(sessionId);
    if (!state) return {MaiErrorCode::NotFound, "observe a window in this Session first"};
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->token.empty() || state->token != snapshotId ||
        state->window.id != windowId ||
        std::chrono::steady_clock::now() - state->observedAt >
            std::chrono::minutes(5))
        return {MaiErrorCode::InvalidInput, "window observation expired; observe again"};
    if ((action == "click" || action == "scroll") &&
        (!ratio(xRatio) || !ratio(yRatio)))
        return {MaiErrorCode::InvalidInput, "window coordinates must be fractions from 0 to 1"};
    if (action == "type" && (text.empty() || text.size() > 4000))
        return {MaiErrorCode::InvalidInput, "type needs 1 to 4000 bytes of text"};
    if (action == "scroll" && (scrollLines == 0 || std::abs(scrollLines) > 10))
        return {MaiErrorCode::InvalidInput, "scroll lines must be between -10 and 10, excluding zero"};
    if (action != "click" && action != "type" && action != "key" && action != "scroll")
        return {MaiErrorCode::InvalidInput, "unknown computer action"};
    const auto listed = maiDesktopWindows();
    if (!listed) return listed.error();
    bool stillVisible = false;
    for (const auto& window : listed.value()) {
        if (window.id == windowId) {
            stillVisible = currentWindowMatches(state->window, window);
            break;
        }
    }
    if (!stillVisible)
        return {MaiErrorCode::InvalidInput, "selected window changed; observe again"};
    const MaiError activated = maiDesktopActivateWindow(state->window);
    if (activated) return activated;
    if (action == "type") {
        const auto secure = maiDesktopFocusedInputIsSecure();
        if (!secure) return secure.error();
        if (secure.value())
            return {MaiErrorCode::NotSupported,
                    "password and other secure inputs need manual user entry"};
    }
    const double x = state->window.x + xRatio *
        (std::max)(1.0, state->window.width - 1);
    const double y = state->window.y + yRatio *
        (std::max)(1.0, state->window.height - 1);
    MaiError result;
    if (action == "click") result = maiDesktopClick(x, y);
    else if (action == "type") result = maiDesktopType(text);
    else if (action == "key") result = maiDesktopKey(key);
    else result = maiDesktopScroll(x, y, scrollLines);
    if (!result) state->token.clear();
    return result;
}

MaiError DesktopComputerController::clear(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sessions.erase(sessionId) == 0)
        return {MaiErrorCode::NotFound, "this Session has no selected window"};
    return {};
}

namespace {

class DesktopComputerTool final : public MaiTool {
public:
    explicit DesktopComputerTool(std::shared_ptr<DesktopComputerController> controller)
        : controller_(std::move(controller)) {}
    std::string name() const override { return "desktop_computer"; }
    std::string description() const override {
        return "Observe one exact visible Desktop window and its bounded Accessibility/UIA "
               "elements; the result includes a screenshot and a snapshot_id. After approval, "
               "click or scroll with 0..1 coordinates relative to that window, type into its "
               "focused non-password input, or send one supported key. The window ID, title and "
               "geometry must still match. Every observation and action needs approval. An "
               "action only dispatches input; observe again to verify its effect. macOS needs "
               "Screen Recording and Accessibility permissions; Windows uses the interactive "
               "desktop. This cannot control secure or locked screens.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["observe","click","type","key","scroll","clear"]},)"
               R"("window_title":{"type":"string"},"include_elements":{"type":"boolean"},)"
               R"("snapshot_id":{"type":"string"},"window_id":{"type":"string"},)"
               R"("x_ratio":{"type":"number","minimum":0,"maximum":1},)"
               R"("y_ratio":{"type":"number","minimum":0,"maximum":1},)"
               R"("text":{"type":"string","maxLength":4000},)"
               R"("key":{"type":"string","enum":["Enter","Tab","Escape","Backspace",)"
               R"("ArrowLeft","ArrowRight","ArrowUp","ArrowDown"]},)"
               R"("lines":{"type":"integer","minimum":-10,"maximum":10}},)"
               R"("required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        return args["action"].get<std::string>() != "clear";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "computer operation canceled");
        if (context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "a local Desktop Session is required");
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "computer action is required");
        auto exact = [&args](std::initializer_list<const char*> allowed) {
            for (auto item = args.begin(); item != args.end(); ++item) {
                bool found = false;
                for (const char* field : allowed) found |= item.key() == field;
                if (!found) return false;
            }
            return true;
        };
        const std::string action = args["action"].get<std::string>();
        if (action == "observe") {
            if (!exact({"action", "window_title", "include_elements"}) ||
                !args.value("window_title", Json{}).is_string() ||
                (args.contains("include_elements") &&
                 !args["include_elements"].is_boolean()) ||
                context.root.empty())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "observe needs an exact window title and workspace");
            if (context.model == "glm-5.3")
                return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                              "the current model cannot inspect screenshots");
            const auto observed = controller_->observe(context.sessionId,
                args["window_title"].get<std::string>(), context.root,
                args.value("include_elements", true));
            if (!observed) return failure(observed.error());
            return MaiToolResult::successWithImages(observed.value().json,
                {MaiToolImage{observed.value().imagePath, "image/png"}});
        }
        if (action == "clear") {
            if (!exact({"action"}))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "clear has no other parameters");
            const MaiError cleared = controller_->clear(context.sessionId);
            if (cleared) return failure(cleared);
            return MaiToolResult::success(R"({"state":"cleared"})");
        }
        if (!args.value("snapshot_id", Json{}).is_string() ||
            !args.value("window_id", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "action needs an observed snapshot and window ID");
        const std::string id = args["window_id"].get<std::string>();
        char* end = nullptr;
        const std::uint64_t windowId = std::strtoull(id.c_str(), &end, 10);
        if (id.empty() || *end != '\0' || windowId == 0)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid window ID");
        const bool positioned = action == "click" || action == "scroll";
        if (!exact(action == "click" || action == "scroll" ?
                   (action == "click" ?
                    std::initializer_list<const char*>{"action", "snapshot_id", "window_id",
                                                       "x_ratio", "y_ratio"} :
                    std::initializer_list<const char*>{"action", "snapshot_id", "window_id",
                                                       "x_ratio", "y_ratio", "lines"}) :
                   (action == "type" ?
                    std::initializer_list<const char*>{"action", "snapshot_id", "window_id", "text"} :
                    std::initializer_list<const char*>{"action", "snapshot_id", "window_id", "key"})))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "unexpected computer action parameter");
        if ((positioned && (!args.value("x_ratio", Json{}).is_number() ||
                            !args.value("y_ratio", Json{}).is_number())) ||
            (action == "type" && !args.value("text", Json{}).is_string()) ||
            (action == "key" && !args.value("key", Json{}).is_string()) ||
            (action == "scroll" && !args.value("lines", Json{}).is_number_integer()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "computer action parameters are incomplete");
        const MaiError acted = controller_->act(context.sessionId, action,
            args["snapshot_id"].get<std::string>(), windowId,
            positioned ? args["x_ratio"].get<double>() : 0,
            positioned ? args["y_ratio"].get<double>() : 0,
            action == "type" ? args["text"].get<std::string>() : std::string(),
            action == "key" ? args["key"].get<std::string>() : std::string(),
            action == "scroll" ? args["lines"].get<int>() : 0);
        if (acted) return failure(acted);
        return MaiToolResult::success(Json{{"state", "dispatched"}, {"action", action},
            {"verified", false}, {"next_step", "Observe the same window again to verify it."}}.dump());
    }

private:
    std::shared_ptr<DesktopComputerController> controller_;
};

} // namespace

void registerDesktopComputerTools(MaiToolRegistry& registry,
    std::shared_ptr<DesktopComputerController> controller) {
    registry.add(std::make_unique<DesktopComputerTool>(std::move(controller)));
}
