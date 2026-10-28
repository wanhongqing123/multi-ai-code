// 文件作用：移动端 C++ Agent 适配层；MaiMobileAgentTests 负责行为与回归验证，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：freeHostToolResponse、releaseHostToolContext。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h、MaiIdGenerator.h、MaiMobileAgent.h；实现与这些接口协作。
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMobileAgent.h"
#include "MaiMobileHostTools.h"
#include "MaiImageFilter.h"
#include "MaiSqliteStore.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <httplib.h>
#include <json.hpp>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#if !defined(_WIN32)
#include <pthread.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
using Json = nlohmann::json;
#if defined(__APPLE__)
static const char* kPhotoAlbumTool = "mobile_photos_add_to_album";
static const char* kCameraTool = "maichat_camera_command";
static const char* kCameraArguments = "{\"action\":\"open\"}";
#elif defined(__ANDROID__)
static const char* kPhotoAlbumTool = "mobile_photos_copy_to_album";
static const char* kCameraTool = "mobile_take_photo";
static const char* kCameraArguments = "{}";
#else
static const char* kPhotoAlbumTool = nullptr;
static const char* kCameraTool = "mobile_take_photo";
static const char* kCameraArguments = "{}";
#endif
#define CHECK(x)                                                      \
    do {                                                              \
        if (!(x)) {                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n",     \
                         __FILE__, __LINE__, #x);                    \
            throw std::runtime_error(#x);                            \
        }                                                             \
    } while (0)
static Json call(void* agent, const Json& value) {
    char* response = maiMobileAgentRequest(agent, value.dump().c_str());
    CHECK(response);
    auto parsed = Json::parse(response);
    maiMobileAgentFree(response);
    return parsed;
}
struct HostToolProbe {
    std::atomic<int> calls{0};
    std::atomic<bool> released{false};
    std::string lastTool;
    std::string lastArguments;
};
static const char* hostToolHandler(void* context, const char* tool, const char* arguments) {
    auto* probe = static_cast<HostToolProbe*>(context);
    probe->lastTool = tool ? tool : "";
    probe->lastArguments = arguments ? arguments : "";
    ++probe->calls;
    const std::string response =
        Json{{"ok", true},
             {"output", probe->lastTool == "maichat_send_text" ? "sent"
                        : probe->lastTool == "mobile_decode_text" ? "\u4f60\u597d" : "file_read"}}
            .dump();
    char* owned = static_cast<char*>(std::malloc(response.size() + 1));
    CHECK(owned != nullptr);
    std::memcpy(owned, response.c_str(), response.size() + 1);
    return owned;
}
static void freeHostToolResponse(void*, const char* response) {
    std::free(const_cast<char*>(response));
}
static void releaseHostToolContext(void* context) {
    static_cast<HostToolProbe*>(context)->released = true;
}
static std::atomic<int> localSessionChanges{0};
static void localSessionChanged(const char* sessionId) {
    if (sessionId != nullptr && std::strncmp(sessionId, "ses_", 4) == 0)
        ++localSessionChanges;
}
static std::string frame(const Json& delta, const char* finish = nullptr) {
    Json choice = {
        {"delta", delta}, {"index", 0}, {"finish_reason", finish ? Json(finish) : Json(nullptr)}};
    return "data: " + Json{{"choices", Json::array({choice})}}.dump() + "\n\n";
}

struct ScreenCaptureProbe {
    std::atomic<int> calls{0};
    std::atomic<bool> released{false};
    std::atomic<bool>* cancel = nullptr;
    bool cancelOnReturn = false;
    Json arguments;
};

static const char* captureHostHandler(void* context, const char* tool, const char* arguments) {
    auto* probe = static_cast<ScreenCaptureProbe*>(context);
    CHECK(tool && std::strcmp(tool, "mobile_capture_screen") == 0);
    probe->arguments = Json::parse(arguments);
    ++probe->calls;
    const std::string output = probe->arguments.at("output_path").get<std::string>();
    CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output),
                                    std::string("\x89PNG\r\n\x1a\n", 8)).hasError());
    if (probe->cancelOnReturn && probe->cancel) probe->cancel->store(true);
    const std::string response = Json{{"ok", true}, {"output", {{"captured", true}}}}.dump();
    auto* owned = static_cast<char*>(std::malloc(response.size() + 1));
    CHECK(owned != nullptr);
    std::memcpy(owned, response.c_str(), response.size() + 1);
    return owned;
}

static void releaseCaptureContext(void* context) {
    static_cast<ScreenCaptureProbe*>(context)->released = true;
}

static void captureContract() {
    struct Workspace {
        MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-mobile-capture-test-" + MaiIdGenerator::newEventId()));
        ~Workspace() { MaiFileSystem::removeRecursively(path); }
    } workspace;
    CHECK(!MaiFileSystem::createDirectories(workspace.path).hasError());
    ScreenCaptureProbe probe;
    auto dispatcher = makeMaiMobileHostDispatcher();
    CHECK(setMaiMobileHostToolHandler(dispatcher, &probe, captureHostHandler,
                                     freeHostToolResponse, releaseCaptureContext));
    MaiToolRegistry registry;
    registerMobileScreenCaptureTools(registry, dispatcher);
    MaiTool* capture = registry.findForModel("mobile_capture_screen");
    CHECK(capture != nullptr);
    const Json schema = Json::parse(capture->parametersSchema());
    CHECK(schema["additionalProperties"] == false);
    std::atomic<bool> cancel{false};
    probe.cancel = &cancel;
    MaiToolContext context;
    context.root = workspace.path.toUtf8();
    context.cancel = &cancel;

#if defined(__APPLE__) && TARGET_OS_IPHONE
    CHECK(schema["properties"]["target"]["enum"] ==
          Json::array({"current_app", "last_external"}));
    CHECK(schema["properties"]["delay_ms"]["type"] == "integer");
    CHECK(schema["properties"]["delay_ms"]["minimum"] == 0);
    CHECK(schema["properties"]["delay_ms"]["maximum"] == 30000);

    // Cancel the actual C++ tool while it is waiting, before any host callback.
    std::optional<MaiToolResult> pending;
    std::exception_ptr workerError;
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        entered.store(true);
        try {
            pending.emplace(capture->execute(
                R"({"target":"current_app","delay_ms":5000})", context));
        } catch (...) { workerError = std::current_exception(); }
        finished.store(true);
    });
    while (!entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool waited = !finished.load();
    const auto canceledAt = std::chrono::steady_clock::now();
    cancel.store(true);
    worker.join();
    if (workerError) std::rethrow_exception(workerError);
    CHECK(waited && pending.has_value() && pending->hasError());
    CHECK(pending->error().code() == MaiErrorCode::Canceled);
    CHECK(pending->images().empty());
    CHECK(std::chrono::steady_clock::now() - canceledAt < std::chrono::seconds(2));
    CHECK(probe.calls.load() == 0);
    int files = 0;
    MaiFileSystem::walk(workspace.path, [&](const MaiFileEntry& entry) {
        if (!entry.isDirectory) ++files;
        return MaiWalkAction::Continue;
    });
    CHECK(files == 0);
    cancel.store(false);

    const auto started = std::chrono::steady_clock::now();
    auto delayed = capture->execute(R"({"target":"current_app","delay_ms":75})", context);
    CHECK(!delayed.hasError() && delayed.images().size() == 1);
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(50));
    CHECK(probe.calls.load() == 1);
    CHECK(probe.arguments["target"] == "current_app" && probe.arguments["delay_ms"] == 0);
    CHECK(probe.arguments.size() == 3);
    CHECK(delayed.images()[0].path == probe.arguments["output_path"]);
    CHECK(!MaiFileSystem::removeFile(MaiFilePath::fromUtf8(delayed.images()[0].path)).hasError());
    auto external = capture->execute(R"({"target":"last_external"})", context);
    CHECK(!external.hasError());
    CHECK(probe.arguments["target"] == "last_external" && probe.arguments["delay_ms"] == 0);
    CHECK(!MaiFileSystem::removeFile(MaiFilePath::fromUtf8(external.images()[0].path)).hasError());
    const int validCalls = probe.calls.load();
    for (const char* invalid : {R"({"target":"entire_device"})", R"({"target":false})",
                               R"({"delay_ms":-1})", R"({"delay_ms":30001})",
                               R"({"delay_ms":0.5})", R"({"delay_ms":true})",
                               R"({"delay_ms":"25"})", R"({"output_path":"outside.png"})"}) {
        auto rejected = capture->execute(invalid, context);
        CHECK(rejected.hasError() && rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(probe.calls.load() == validCalls);
#else
    CHECK(schema["properties"].empty());
    for (const char* invalid : {R"({"target":"current_app"})", R"({"target":"last_external"})",
                               R"({"delay_ms":0})", R"({"delay_ms":75})"}) {
        auto rejected = capture->execute(invalid, context);
        CHECK(rejected.hasError() && rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(probe.calls.load() == 0);
#endif

    auto normal = capture->execute("{}", context);
    CHECK(!normal.hasError() && normal.images().size() == 1);
    CHECK(!probe.arguments.contains("target"));
#if defined(__APPLE__) && TARGET_OS_IPHONE
    CHECK(probe.arguments["delay_ms"] == 0);
#else
    CHECK(!probe.arguments.contains("delay_ms"));
    CHECK(probe.arguments.size() == 1);
#endif
    const auto output = MaiFilePath::fromUtf8(normal.images()[0].path);
    CHECK(normal.images()[0].mimeType == "image/png");
#if defined(__ANDROID__)
    CHECK(output.dirName() == workspace.path.append(MaiFilePath::fromUtf8("no_backup/screenshots")));
#else
    CHECK(output.dirName() == workspace.path.append(MaiFilePath::fromUtf8("screenshots")));
#endif
    CHECK(MaiFileSystem::exists(output));
    CHECK(!MaiFileSystem::removeFile(output).hasError());

    // A late cancellation must remove only this call's newly written PNG.
    const auto retained = output.dirName().append(MaiFilePath::fromUtf8("mobile_screen_retained.png"));
    CHECK(!MaiFileSystem::writeFile(retained, "retained").hasError());
    probe.cancelOnReturn = true;
    const int callsBeforeCancel = probe.calls.load();
    auto canceled = capture->execute("{}", context);
    CHECK(canceled.hasError() && canceled.error().code() == MaiErrorCode::Canceled);
    CHECK(canceled.images().empty());
    CHECK(probe.calls.load() == callsBeforeCancel + 1);
    CHECK(!MaiFileSystem::exists(MaiFilePath::fromUtf8(
        probe.arguments["output_path"].get<std::string>())));
    std::string retainedBytes;
    CHECK(!MaiFileSystem::readFile(retained, retainedBytes).hasError() && retainedBytes == "retained");
    auto alreadyCanceled = capture->execute("{}", context);
    CHECK(alreadyCanceled.hasError() && alreadyCanceled.error().code() == MaiErrorCode::Canceled);
    CHECK(probe.calls.load() == callsBeforeCancel + 1);
    clearMaiMobileHostToolHandler(dispatcher);
    CHECK(probe.released.load());
    std::puts("PASS: screen capture schema, dispatch, delay cancellation and owned PNG cleanup");
}
// 作用：按测试场景构造移动 Agent、调用公开接口并核对结果。
// 按真实调用顺序检查输入和可观察结果，失败时保留定位所需的上下文。
int main(int argc, char** argv) {
    const bool captureOnly = argc == 2 && std::strcmp(argv[1], "--capture-contract-only") == 0;
    if (argc != 1 && !captureOnly) {
        std::fputs("usage: MaiMobileAgentTests [--capture-contract-only]\n", stderr);
        return 2;
    }
    try { captureContract(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: screen capture contract: %s\n", error.what());
        return 1;
    }
    if (captureOnly) return 0;
    MaiToolRegistry hostRegistry;
    registerMaiChatHostTools(hostRegistry, makeMaiMobileHostDispatcher());
    MaiTool* remoteList = hostRegistry.find("maichat_remote_session_list");
    MaiTool* remoteCreate = hostRegistry.find("maichat_remote_session_create");
    MaiTool* remoteDelete = hostRegistry.find("maichat_remote_session_delete");
    MaiTool* remoteWorkspace = hostRegistry.find("maichat_remote_session_set_workspace");
    CHECK(remoteList != nullptr && remoteCreate != nullptr && remoteDelete != nullptr &&
          remoteWorkspace != nullptr);
    CHECK(!remoteList->requiresApproval("{}"));
    CHECK(!remoteCreate->requiresApproval("{}"));
    CHECK(remoteDelete->requiresApproval("{}"));
    CHECK(remoteDelete->requiresPerCallApproval("{}"));
    CHECK(remoteWorkspace->requiresApproval("{}"));
    CHECK(remoteWorkspace->requiresPerCallApproval("{}"));
#if !defined(_WIN32)
    const unsigned char pixels[] = {255, 0, 0, 255, 0, 255, 0, 255};
    auto flipped = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"flip_horizontal"})");
    CHECK(flipped.error == nullptr);
    CHECK(flipped.rgba != nullptr && flipped.width == 2 && flipped.height == 1);
    CHECK(flipped.rgba[0] == 0 && flipped.rgba[1] == 255);
    CHECK(flipped.rgba[4] == 255 && flipped.rgba[5] == 0);
    maiImageFilterFree(flipped.rgba);
    auto cropped = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"crop","x":1,"y":0,"width":1,"height":1})");
    CHECK(cropped.error == nullptr && cropped.width == 1 && cropped.height == 1);
    CHECK(cropped.rgba[0] == 0 && cropped.rgba[1] == 255);
    maiImageFilterFree(cropped.rgba);
    auto rotated = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"rotate","degrees":90})");
    CHECK(rotated.error == nullptr && rotated.width == 1 && rotated.height == 2);
    maiImageFilterFree(rotated.rgba);
    auto resized = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"resize","width":4,"height":2})");
    CHECK(resized.error == nullptr && resized.width == 4 && resized.height == 2);
    maiImageFilterFree(resized.rgba);
    auto grayscale = maiImageFilterRgba(pixels, 2, 1, 8,
                                        R"({"operation":"grayscale"})");
    CHECK(grayscale.error == nullptr && grayscale.width == 2 && grayscale.height == 1);
    CHECK(std::abs(int(grayscale.rgba[0]) - int(grayscale.rgba[1])) <= 2);
    maiImageFilterFree(grayscale.rgba);
    auto adjusted = maiImageFilterRgba(pixels, 2, 1, 8,
                                       R"({"operation":"adjust","brightness":0.1,"contrast":1.1,"saturation":1.2})");
    CHECK(adjusted.error == nullptr && adjusted.width == 2 && adjusted.height == 1);
    maiImageFilterFree(adjusted.rgba);
    auto sharpened = maiImageFilterRgba(pixels, 2, 1, 8,
                                        R"({"operation":"sharpen","amount":1})");
    CHECK(sharpened.error == nullptr && sharpened.width == 2 && sharpened.height == 1);
    maiImageFilterFree(sharpened.rgba);
    auto beautified = maiImageFilterRgba(pixels, 2, 1, 8,
                                         R"({"operation":"beautify","strength":0.5})");
    CHECK(beautified.error == nullptr && beautified.width == 2 && beautified.height == 1);
    maiImageFilterFree(beautified.rgba);
    auto invalid = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"crop","x":2,"y":0,"width":1,"height":1})");
    CHECK(invalid.rgba == nullptr && invalid.error != nullptr);
    maiImageFilterFree(invalid.error);
#endif

    httplib::Server server;
    // 本地假模型端点核对 Agent 发出的请求体，并返回可控的流式事件。
    // 测试只连环回地址，不会向真实模型供应商提交任务。
    server.Post("/chat/completions", [](const httplib::Request& request,
                                        httplib::Response& response) {
        const Json body = Json::parse(request.body);
        CHECK(request.get_header_value("Authorization") == "Bearer test-key");
        CHECK(body["messages"].front()["role"] == "system");
        const bool replySuggestion = body["messages"].front()["content"].get<std::string>().find(
                                         "reply suggestions") != std::string::npos;
        if (replySuggestion) {
            CHECK(!body.contains("tools"));
            response.set_content(
                frame({{"content",
                        R"({"natural":"好的","casual":"收到 😄","professional":"已收到，我会尽快处理。"})"}},
                      "stop") +
                    "data: [DONE]\n\n",
                "text/event-stream");
            return;
        }
        CHECK(body["messages"].front()["content"].get<std::string>().find(
                  "GitHub-Flavored Markdown") != std::string::npos);
        const std::string instructions = body["messages"].front()["content"].get<std::string>();
        CHECK(instructions.find("Seedance") == std::string::npos);
        CHECK(instructions.find("Seedream") == std::string::npos);
        CHECK(instructions.find("ark_assets") == std::string::npos);
        // 移动端不能向模型宣称可以执行桌面 shell。
        bool hasCreateFile = false, hasCreateDirectory = false, hasDeleteFile = false, hasViewImage = false,
             hasMaiChatContacts = false, hasMaiChatRequestFriend = false, hasMaiChatNickname = false,
             hasMaiChatSend = false, hasMaiChatBroadcast = false, hasGeneratePdf = false, hasMobilePhotos = false,
             hasMobileAlbums = false, hasMobilePhotoRead = false, hasMobilePhotoOriginal = false,
             hasMobileMediaOriginal = false, hasMobilePhotoSave = false, hasMobileTransform = false,
             hasMobileBeautify = false, hasFfprobe = false, hasMobilePreview = false, hasFaceDetection = false,
             hasPersonSegmentation = false, hasMobilePhotoAlbumWrite = false, hasPermissionRequest = false,
             hasSceneDetect = false, hasMotionDetect = false, hasDownloadFile = false, hasLocation = false,
             hasMaiChatMedia = false, hasVideoMatting = false, hasAgentMedia = false, hasSeedance = false,
             hasSeedream = false, hasArkAssets = false, hasCamera = false, hasLocalMusic = false, hasSessionList = false,
             hasSessionCreate = false, hasSessionDelete = false, hasLocalSessionWorkspace = false,
             hasRemoteSessionList = false, hasRemoteSessionCreate = false, hasRemoteSessionDelete = false,
             hasRemoteSessionWorkspace = false, hasMobileBrowser = false,
             hasScreenShareStart = false, hasScreenShareStop = false,
             hasScreenCapture = false;
        for (const auto& tool : body["tools"]) {
            CHECK(tool["function"]["name"] != "shell");
            if (tool["function"]["name"] == "file_create") hasCreateFile = true;
            if (tool["function"]["name"] == "file_create_directory") hasCreateDirectory = true;
            if (tool["function"]["name"] == "file_delete") hasDeleteFile = true;
            if (tool["function"]["name"] == "session_list") hasSessionList = true;
            if (tool["function"]["name"] == "session_create") hasSessionCreate = true;
            if (tool["function"]["name"] == "session_delete") hasSessionDelete = true;
            if (tool["function"]["name"] == "session_set_workspace") hasLocalSessionWorkspace = true;
            if (tool["function"]["name"] == "maichat_remote_session_list") hasRemoteSessionList = true;
            if (tool["function"]["name"] == "maichat_remote_session_create") hasRemoteSessionCreate = true;
            if (tool["function"]["name"] == "maichat_remote_session_delete") {
                hasRemoteSessionDelete = true;
                const auto& schema = tool["function"]["parameters"];
                CHECK(schema["required"] == Json::array({"device_slot", "session_id"}));
            }
            if (tool["function"]["name"] == "maichat_remote_session_set_workspace") {
                hasRemoteSessionWorkspace = true;
                const auto& schema = tool["function"]["parameters"];
                CHECK(schema["required"] ==
                      Json::array({"device_slot", "session_id", "workspace_path"}));
            }
            if (tool["function"]["name"] == "mobile_browser") hasMobileBrowser = true;
            if (tool["function"]["name"] == "mobile_start_screen_share") hasScreenShareStart = true;
            if (tool["function"]["name"] == "mobile_stop_screen_share") hasScreenShareStop = true;
            if (tool["function"]["name"] == "mobile_capture_screen") hasScreenCapture = true;
            if (tool["function"]["name"] == "view_image") hasViewImage = true;
            if (tool["function"]["name"] == "generate_pdf") hasGeneratePdf = true;
            if (tool["function"]["name"] == "maichat_list_contacts") hasMaiChatContacts = true;
            if (tool["function"]["name"] == "maichat_request_friend") hasMaiChatRequestFriend = true;
            if (tool["function"]["name"] == "maichat_set_nickname") hasMaiChatNickname = true;
            if (tool["function"]["name"] == "maichat_send_text") hasMaiChatSend = true;
            if (tool["function"]["name"] == "maichat_send_media") hasMaiChatMedia = true;
            if (tool["function"]["name"] == "agent_send_media") hasAgentMedia = true;
            if (tool["function"]["name"] == "seedance_video") hasSeedance = true;
            if (tool["function"]["name"] == "seedream_image") hasSeedream = true;
            if (tool["function"]["name"] == "ark_assets") hasArkAssets = true;
            if (tool["function"]["name"] == "curl") hasDownloadFile = true;
            if (tool["function"]["name"] == "maichat_broadcast_text")
                hasMaiChatBroadcast = true;
            if (tool["function"]["name"] == "mobile_list_photos") hasMobilePhotos = true;
            if (tool["function"]["name"] == "mobile_take_photo" ||
                tool["function"]["name"] == "maichat_camera_command") hasCamera = true;
            if (tool["function"]["name"] == "mobile_play_local_music") hasLocalMusic = true;
            if (tool["function"]["name"] == "mobile_list_albums") hasMobileAlbums = true;
            if (tool["function"]["name"] == "mobile_read_photo") hasMobilePhotoRead = true;
            if (tool["function"]["name"] == "mobile_export_photo_original")
                hasMobilePhotoOriginal = true;
            if (tool["function"]["name"] == "mobile_export_media_original")
                hasMobileMediaOriginal = true;
            if (tool["function"]["name"] == "mobile_save_image") hasMobilePhotoSave = true;
            if (tool["function"]["name"] == "mobile_transform_image")
                hasMobileTransform = true;
            if (tool["function"]["name"] == "mobile_beautify_image")
                hasMobileBeautify = true;
            if (tool["function"]["name"] == "ffprobe") hasFfprobe = true;
            if (tool["function"]["name"] == "mobile_detect_faces") hasFaceDetection = true;
            if (tool["function"]["name"] == "mobile_segment_person") hasPersonSegmentation = true;
            if (tool["function"]["name"] == "mobile_preview_image") hasMobilePreview = true;
            if (tool["function"]["name"] == "mobile_request_permission")
                hasPermissionRequest = true;
            if (tool["function"]["name"] == "mobile_get_location") hasLocation = true;
            if (tool["function"]["name"] == "cv_scene_detect") hasSceneDetect = true;
            if (tool["function"]["name"] == "cv_motion_detect") hasMotionDetect = true;
            if (tool["function"]["name"] == "cv_video_matting") hasVideoMatting = true;
            if (kPhotoAlbumTool != nullptr && tool["function"]["name"] == kPhotoAlbumTool)
                hasMobilePhotoAlbumWrite = true;
        }
        CHECK(hasCreateFile);
        CHECK(hasCreateDirectory);
        CHECK(hasDeleteFile);
        CHECK(hasSessionList && hasSessionCreate && hasSessionDelete);
        CHECK(!hasLocalSessionWorkspace);
        CHECK(hasRemoteSessionList && hasRemoteSessionCreate && hasRemoteSessionDelete);
        CHECK(hasRemoteSessionWorkspace);
        CHECK(hasMobileBrowser);
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        CHECK(hasScreenShareStart && hasScreenShareStop && hasScreenCapture);
#else
        CHECK(!hasScreenShareStart && !hasScreenShareStop && !hasScreenCapture);
#endif
        CHECK(hasViewImage);
        CHECK(hasGeneratePdf);
        CHECK(hasMaiChatContacts);
        CHECK(hasMaiChatRequestFriend);
        CHECK(hasMaiChatNickname);
        CHECK(hasMaiChatSend);
        CHECK(hasMaiChatMedia);
        CHECK(hasAgentMedia);
        CHECK(hasCamera);
        CHECK(hasLocalMusic);
        CHECK(!hasSeedance);
        CHECK(!hasSeedream);
        CHECK(!hasArkAssets);
        CHECK(hasDownloadFile);
        CHECK(hasMaiChatBroadcast);
        CHECK(hasMobilePhotos);
        CHECK(hasMobileAlbums);
        CHECK(hasMobilePhotoRead);
        CHECK(hasMobilePhotoOriginal);
        CHECK(hasMobileMediaOriginal);
        CHECK(hasMobilePhotoSave);
        CHECK(hasMobileTransform);
        CHECK(hasMobileBeautify);
        CHECK(hasFfprobe);
        CHECK(hasFaceDetection);
        CHECK(hasPersonSegmentation);
        CHECK(hasMobilePreview);
        CHECK(hasPermissionRequest);
        CHECK(hasLocation);
        CHECK(hasSceneDetect);
        CHECK(hasMotionDetect);
        CHECK(hasVideoMatting);
        if (kPhotoAlbumTool != nullptr) CHECK(hasMobilePhotoAlbumWrite);
        const auto& last = body["messages"].back();
        std::string input;
        if (last["content"].is_string()) {
            input = last["content"].get<std::string>();
        } else if (last["content"].is_array()) {
            for (const auto& part : last["content"]) {
                if (part.value("type", "") == "text") input = part.value("text", "");
                if (part.value("type", "") == "image_url")
                    CHECK(part["image_url"].value("url", "") == "data:image/png;base64,iVBORw==");
            }
        }
        if (input == "error") {
            response.status = 429;
            response.set_content(R"({"error":{"message":"quota exhausted"}})", "application/json");
            return;
        }
        if (input == "file_write") {
            Json invocation = {
                {"index", 0},
                {"id", "call_write"},
                {"type", "function"},
                {"function",
                 {{"name", "file_write"},
                  {"arguments", "{\"path\":\"result.txt\",\"content\":\"saved\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
            return;
        }
        if (input == "read-app-sibling") {
            Json invocation = {
                {"index", 0}, {"id", "call_read_sibling"}, {"type", "function"},
                {"function", {{"name", "file_read"},
                              {"arguments", "{\"path\":\"../shared.txt\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "read-legacy") {
            Json invocation = {
                {"index", 0}, {"id", "call_read_legacy"}, {"type", "function"},
                {"function", {{"name", "file_read"},
                              {"arguments", "{\"path\":\"legacy.txt\",\"encoding\":\"gb18030\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "host-send") {
            Json invocation = {
                {"index", 0},
                {"id", "call_host_send"},
                {"type", "function"},
                {"function",
                 {{"name", "maichat_send_text"},
                  {"arguments", "{\"peer_id\":\"alice\",\"text\":\"hello\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
            return;
        }
        if (input == "camera-open") {
            Json invocation = {
                {"index", 0}, {"id", "call_camera_open"}, {"type", "function"},
                {"function", {{"name", kCameraTool}, {"arguments", kCameraArguments}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
            return;
        }
        if (input == "music-open") {
            Json invocation = {
                {"index", 0}, {"id", "call_music_open"}, {"type", "function"},
                {"function", {{"name", "mobile_play_local_music"}, {"arguments", "{}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
            return;
        }
        if (input == "photo-add") {
            Json invocation = {
                {"index", 0}, {"id", "call_photo_add"}, {"type", "function"},
                 {"function", {{"name", kPhotoAlbumTool == nullptr ? "unused" : kPhotoAlbumTool},
                              {"arguments", "{\"album_name\":\"Trip\",\"photo_ids\":[\"one\"]}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "photo-save") {
            Json invocation = {
                {"index", 0}, {"id", "call_photo_save"}, {"type", "function"},
                {"function", {{"name", "mobile_save_image"},
                              {"arguments", "{\"path\":\"created.png\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "photo-transform") {
            Json invocation = {
                {"index", 0}, {"id", "call_photo_transform"}, {"type", "function"},
                {"function", {{"name", "mobile_transform_image"},
                              {"arguments", "{\"path\":\"photo.jpg\",\"operation\":\"beautify\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "photo-preview") {
            Json invocation = {
                {"index", 0}, {"id", "call_photo_preview"}, {"type", "function"},
                {"function", {{"name", "mobile_preview_image"},
                              {"arguments", "{\"path\":\"created.png\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        const bool slow = input == "stop";
        response.set_chunked_content_provider(
            "text/event-stream", [slow, step = 0](size_t, httplib::DataSink& sink) mutable {
                std::string text;
                if (step == 0)
                    text = frame({{"reasoning_content", "draft"}});
                else if (step == 1)
                    text = frame({{"content", "hello "}});
                else {
                    text = frame({{"content", "world"}}, "stop") + "data: [DONE]\n\n";
                }
                if (!sink.write(text.data(), text.size())) return false;
                if (step++ >= 2) {
                    sink.done();
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(slow ? 1600 : 160));
                return true;
            });
    });
    int port = server.bind_to_any_port("127.0.0.1");
    std::thread listener([&] {
#if defined(__APPLE__)
        pthread_setname_np("MaiMobileHTTP");
#elif !defined(_WIN32)
        pthread_setname_np(pthread_self(), "MaiMobileHTTP");
#endif
        server.listen_after_bind();
    });
    const MaiFilePath testRoot = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-mobile-test-" + MaiIdGenerator::newEventId()));
    CHECK(!MaiFileSystem::createDirectories(testRoot).hasError());
    const std::string directory = testRoot.toUtf8();
    HostToolProbe browserProbe;
    auto browserDispatcher = makeMaiMobileHostDispatcher();
    CHECK(setMaiMobileHostToolHandler(browserDispatcher, &browserProbe,
        hostToolHandler, freeHostToolResponse, releaseHostToolContext));
    MaiToolRegistry browserRegistry;
    registerMobileBrowserTools(browserRegistry, browserDispatcher);
    MaiTool* browser = browserRegistry.findForModel("mobile_browser");
    CHECK(browser != nullptr);
    CHECK(browser->requiresPerCallApproval(R"({"action":"open"})"));
    CHECK(browser->requiresPerCallApproval(R"({"action":"click"})"));
    CHECK(browser->requiresPerCallApproval(R"({"action":"screenshot"})"));
    CHECK(!browser->requiresApproval(R"({"action":"observe"})"));
    MaiToolContext browserContext;
    browserContext.sessionId = "ses_browser";
    browserContext.root = directory;
    CHECK(browser->execute(R"({"action":"open","url":"file:///private/test"})",
                           browserContext).hasError());
    CHECK(browserProbe.calls.load() == 0);
    CHECK(!browser->execute(R"({"action":"open","url":"https://example.com"})",
                            browserContext).hasError());
    CHECK(browserProbe.lastTool == "mobile_browser" &&
          Json::parse(browserProbe.lastArguments)["session_id"] == "ses_browser");
    clearMaiMobileHostToolHandler(browserDispatcher);
    CHECK(browserProbe.released.load());
    void* agent = maiMobileAgentCreate();
    int fakeOrtApi = 0;
    HostToolProbe hostProbe;
    int status = 0;
    try {
        CHECK(maiMobileAgentSetChangeHandler(agent, localSessionChanged) == 1);
        CHECK(maiMobileAgentSetHostToolHandler(agent, &hostProbe, nullptr, nullptr, nullptr) == 0);
        CHECK(maiMobileAgentSetHostToolHandler(agent, &hostProbe, hostToolHandler,
                                               freeHostToolResponse,
                                               releaseHostToolContext) == 1);
        CHECK(maiMobileAgentSetOrtApiBase(agent, &fakeOrtApi) == 1);
        const std::string modelPath = directory + "/rvm.onnx";
        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(modelPath), "model"));
        Json config = {
            {"op", "configure"},      {"baseUrl", "http://127.0.0.1:" + std::to_string(port)},
            {"apiKey", "test-key"},   {"model", "test"},
            {"workspace", directory}, {"database", directory + "/sessions.db"},
            {"rvmModelPath", modelPath}};
#if defined(__APPLE__) && TARGET_OS_IPHONE
        config["supportsScreenCapture"] = true;
#endif
        CHECK(call(agent, config)["ok"] == true);
        const Json suggestions = call(
            agent,
            {{"op", "suggest_replies"},
             {"messages", Json::array({{{"speaker", "friend"}, {"text", "今天能完成吗？"}}})}});
        CHECK(suggestions["natural"] == "好的");
        CHECK(suggestions["casual"] == "收到 😄");
        CHECK(suggestions["professional"] == "已收到，我会尽快处理。");
        const int initialHostCalls = hostProbe.calls.load();
        std::string session = call(agent, {{"op", "create"}}).at("id");
        CHECK(localSessionChanges.load() > 0);
        const auto remotePage = call(agent, {{"op", "remote_snapshot"}, {"session", session},
                                             {"limit", 2}});
        CHECK(remotePage["ok"] == true && remotePage["messages"].empty());
        CHECK(call(agent, {{"op", "snapshot"}, {"session", session}})["changed"] == true);
        auto snapshot = [&] {
            return call(agent, {{"op", "snapshot"}, {"session", session}, {"force", true}});
        };
        auto wait = [&](const auto& condition) {
            Json last;
            for (int i = 0; i < 160; ++i) {
                auto s = snapshot();
                if (condition(s)) return s;
                last = std::move(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            throw std::runtime_error("timed out waiting for native state: " +
                                     last.value("error", std::string{}));
        };
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "host-send"}})["ok"] ==
              true);
        const auto hostPermission =
            wait([](const Json& s) { return !s["permissions"].empty(); });
        CHECK(hostProbe.calls == initialHostCalls);
        CHECK(hostPermission["permissions"][0]["tool"] == "maichat_send_text");
        CHECK(hostPermission["permissions"][0]["allowForSession"] == false);
        CHECK(call(agent,
                   {{"op", "permission"},
                    {"id", hostPermission["permissions"][0]["id"]},
                    {"decision", "approved"}})["ok"] == true);
        wait([](const Json& s) { return s["busy"] == false; });
        CHECK(hostProbe.calls == initialHostCalls + 1);
        CHECK(hostProbe.lastTool == "maichat_send_text");
        CHECK(Json::parse(hostProbe.lastArguments)["peer_id"] == "alice");
        if (kPhotoAlbumTool != nullptr) {
            CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-add"}})["ok"] == true);
            const auto photoDone = wait([](const Json& s) { return s["busy"] == false; });
            CHECK(photoDone["permissions"].empty());
            CHECK(hostProbe.calls == initialHostCalls + 2);
            CHECK(hostProbe.lastTool == kPhotoAlbumTool);
        }
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-save"}})["ok"] == true);
        const auto photoSaved = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoSaved["permissions"].empty());
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 2 : 3));
        CHECK(hostProbe.lastTool == "mobile_save_image");
        CHECK(Json::parse(hostProbe.lastArguments)["path"] == "created.png");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-transform"}})["ok"] == true);
        const auto photoTransformed = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoTransformed["permissions"].empty());
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 3 : 4));
        CHECK(hostProbe.lastTool == "mobile_transform_image");
        CHECK(Json::parse(hostProbe.lastArguments)["operation"] == "beautify");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-preview"}})["ok"] == true);
        const auto photoPreviewed = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoPreviewed["permissions"].empty());
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 4 : 5));
        CHECK(hostProbe.lastTool == "mobile_preview_image");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "camera-open"}})["ok"] == true);
        const auto cameraDone = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(cameraDone["permissions"].empty());
        CHECK(hostProbe.lastTool == kCameraTool);
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 5 : 6));
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "music-open"}})["ok"] == true);
        const auto musicDone = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(musicDone["permissions"].empty());
        CHECK(hostProbe.lastTool == "mobile_play_local_music");
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 6 : 7));
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "hello"}})["ok"] == true);
        CHECK(call(agent, config)["ok"] == false);  // 工作中不能销毁并换配置。
        auto partial = wait([](const Json& s) {
            return s["busy"] == true && s.dump().find("hello ") != std::string::npos;
        });
        CHECK(partial.dump().find("reasoning") != std::string::npos);
        auto done = wait([](const Json& s) { return s["busy"] == false; });
        const auto latestPage = call(agent, {{"op", "snapshot"}, {"session", session},
                                             {"force", true}, {"messageLimit", 1}});
        CHECK(latestPage["messages"].size() == 1);
        CHECK(latestPage["messages"][0]["id"] == done["messages"].back()["id"]);
        const auto latestId = latestPage["messages"][0]["id"].get<std::string>();
        const auto olderPage = call(agent, {{"op", "messages_page"}, {"session", session},
                                            {"before", latestId}, {"limit", 2}});
        CHECK(olderPage["messages"].size() == 2);
        CHECK(olderPage["messages"][0]["id"] < latestId);
        CHECK(olderPage["messages"][1]["id"] < olderPage["messages"][0]["id"]);
        const auto sessionOnly = call(agent, {{"op", "snapshot"}, {"session", session},
                                              {"force", true}, {"messageLimit", 0}});
        CHECK(sessionOnly["messages"].empty());
        CHECK(!sessionOnly["sessions"].empty());
        for (const auto& part : done["messages"].back()["parts"]) {
            if (part["kind"] == "text")
                CHECK(part["text"] == "hello world");
            else if (part["kind"] == "reasoning")
                CHECK(part["text"] == "draft");
            else
                CHECK(false);
        }
        CHECK(done.dump().find("test-key") == std::string::npos);
        CHECK(
            !MaiFileSystem::writeFile(MaiFilePath::fromUtf8(std::string(directory) + "/photo.png"),
                                      std::string("\x89PNG", 4)));
        Json imageRequest = {{"op", "send"}, {"session", session}, {"text", "image"}};
        imageRequest["images"] =
            Json::array({Json{{"path", "photo.png"}, {"mimeType", "image/png"}}});
        CHECK(call(agent, imageRequest)["ok"] == true);
        auto imageDone = wait([](const Json& s) { return s["busy"] == false; });
        bool foundImage = false;
        for (const auto& message : imageDone["messages"])
            for (const auto& part : message["parts"])
                if (part["kind"] == "image" && part["path"] == "photo.png" &&
                    part["mimeType"] == "image/png")
                    foundImage = true;
        CHECK(foundImage);

        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(directory + "/clip.mp4"),
                                        "video fixture"));
        Json videoRequest = { {"op", "send"}, {"session", session},
                              {"text", "Please inspect this video"},
                              {"videos", Json::array({{{"path", "clip.mp4"},
                                                       {"mimeType", "video/mp4"}}})} };
        CHECK(call(agent, videoRequest)["ok"] == true);
        const auto videoDone = wait([](const Json& s) { return s["busy"] == false; });
        bool foundVideo = false;
        for (const auto& message : videoDone["messages"])
            for (const auto& part : message["parts"])
                if (part["kind"] == "video" && part["path"] == "clip.mp4" &&
                    part["mimeType"] == "video/mp4")
                    foundVideo = true;
        CHECK(foundVideo);

        // iOS 升级或恢复后，持久化数据库会跟着数据迁移，但应用容器的绝对路径
        // 可能变化。把同一张相对路径图片放进新工作区，再确认下一轮会先迁移
        // session.directory，历史图片不会继续从旧容器读取。
        const std::string movedWorkspace = std::string(directory) + "/moved-workspace";
        CHECK(!MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(movedWorkspace)));
        CHECK(!MaiFileSystem::writeFile(
            MaiFilePath::fromUtf8(movedWorkspace + "/photo.png"), std::string("\x89PNG", 4)));
        CHECK(!MaiFileSystem::removeFile(
            MaiFilePath::fromUtf8(std::string(directory) + "/photo.png")));
        config["workspace"] = movedWorkspace;
        config["appRoot"] = directory;
        CHECK(call(agent, config)["ok"] == true);
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "relocated"}})["ok"] ==
              true);
        const auto relocated = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(relocated.value("error", "").empty());

        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(directory + "/shared.txt"),
                                        "app sibling content"));
        CHECK(call(agent, { {"op", "send"}, {"session", session},
                            {"text", "read-app-sibling"} })["ok"] == true);
        const auto sibling = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(sibling.dump().find("app sibling content") != std::string::npos);

        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(movedWorkspace + "/legacy.txt"),
                                        std::string("\xC4\xE3\xBA\xC3", 4)));
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "read-legacy"}})["ok"] == true);
        const auto decoded = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(decoded.dump().find("\u4f60\u597d") != std::string::npos);
        CHECK(hostProbe.lastTool == "mobile_decode_text");
        CHECK(Json::parse(hostProbe.lastArguments).value("encoding", "") == "gb18030");
        CHECK(Json::parse(hostProbe.lastArguments).value("base64", "") == "xOO6ww==");

        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "file_write"}})["ok"] == true);
        auto pending = wait([](const Json& s) { return !s["permissions"].empty(); });
        std::string permission = pending["permissions"][0]["id"];
        CHECK(!MaiFileSystem::exists(MaiFilePath::fromUtf8(movedWorkspace + "/result.txt")));
        CHECK(call(agent,
                   {{"op", "permission"}, {"id", permission}, {"decision", "unknown"}})["ok"] ==
              false);
        CHECK(snapshot()["permissions"].size() == 1);
        CHECK(call(agent,
                   {{"op", "permission"}, {"id", permission}, {"decision", "approved"}})["ok"] ==
              true);
        wait([](const Json& s) { return s["busy"] == false; });
        CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(movedWorkspace + "/result.txt")));
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "stop"}})["ok"] == true);
        wait([](const Json& s) {
            return s["busy"] == true && !s["messages"].back()["parts"].empty();
        });
        CHECK(call(agent, {{"op", "stop"}, {"session", session}})["ok"] == true);
        wait([](const Json& s) { return s["busy"] == false; });
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "error"}})["ok"] == true);
        auto failure = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(failure["error"].get<std::string>().find("quota exhausted") != std::string::npos);
        auto messageCount = failure["messages"].size();
        maiMobileAgentDestroy(agent);
        agent = nullptr;
        // 模拟进程被系统终止后残留的未完成记录；不能把它显示成仍在运行。
        {
            auto saved = makeMaiSqliteStore(std::string(directory) + "/sessions.db");
            CHECK(saved);
            MaiMessage orphan;
            orphan.id = MaiIdGenerator::newMessageId();
            orphan.role = MaiRole::Assistant;
            orphan.created = MaiTime::getCurrentTime();
            saved.value()->putMessage(session, orphan);
        }
        agent = maiMobileAgentCreate();
        CHECK(maiMobileAgentSetChangeHandler(agent, localSessionChanged) == 1);
        CHECK(call(agent, config)["ok"] == true);
        CHECK(snapshot()["messages"].size() == messageCount + 1);
        CHECK(snapshot()["messages"].back()["completed"] == 0);
        CHECK(snapshot()["messages"].back()["active"] == false);
        CHECK(call(agent, {{"op", "clear"}, {"session", session}})["ok"] == true);
        CHECK(snapshot()["messages"].empty());
        CHECK(call(agent, {{"op", "delete"}, {"session", session}})["ok"] == true);
        CHECK(snapshot()["sessions"].empty());
        std::puts(
            "PASS: streaming, part kinds, permissions, interruption, "
            "provider errors, persistence, clear/delete");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        status = 1;
    }
    maiMobileAgentDestroy(agent);
    CHECK(hostProbe.released);
    server.stop();
    listener.join();
    MaiFileSystem::removeRecursively(testRoot);
    return status;
}
