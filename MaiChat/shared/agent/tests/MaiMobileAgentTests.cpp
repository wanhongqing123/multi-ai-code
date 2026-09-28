#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMobileAgent.h"
#include "MaiSqliteStore.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <httplib.h>
#include <json.hpp>
#include <stdexcept>
#include <thread>
using Json = nlohmann::json;
#if defined(__APPLE__)
static const char* kPhotoAlbumTool = "mobile_photos_add_to_album";
#elif defined(__ANDROID__)
static const char* kPhotoAlbumTool = "mobile_photos_copy_to_album";
#else
static const char* kPhotoAlbumTool = nullptr;
#endif
#define CHECK(x)                                \
    do {                                        \
        if (!(x)) throw std::runtime_error(#x); \
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
        Json{{"ok", true}, {"output", probe->lastTool == "maichat_send_text" ? "sent" : "read"}}
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
static std::string frame(const Json& delta, const char* finish = nullptr) {
    Json choice = {
        {"delta", delta}, {"index", 0}, {"finish_reason", finish ? Json(finish) : Json(nullptr)}};
    return "data: " + Json{{"choices", Json::array({choice})}}.dump() + "\n\n";
}
int main() {
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

    httplib::Server server;
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
        // 移动端不能向模型宣称可以执行桌面 shell。
        bool hasViewImage = false, hasMaiChatContacts = false, hasMaiChatSend = false,
             hasMaiChatBroadcast = false, hasGeneratePdf = false,
             hasMobilePhotos = false, hasMobileAlbums = false,
             hasMobilePhotoRead = false, hasMobilePhotoOriginal = false,
             hasMobilePhotoSave = false, hasMobileTransform = false,
             hasMobileBeautify = false, hasMobileImageInfo = false, hasMobilePreview = false,
             hasMobilePhotoAlbumWrite = false;
        for (const auto& tool : body["tools"]) {
            CHECK(tool["function"]["name"] != "shell");
            if (tool["function"]["name"] == "view_image") hasViewImage = true;
            if (tool["function"]["name"] == "generate_pdf") hasGeneratePdf = true;
            if (tool["function"]["name"] == "maichat_list_contacts") hasMaiChatContacts = true;
            if (tool["function"]["name"] == "maichat_send_text") hasMaiChatSend = true;
            if (tool["function"]["name"] == "maichat_broadcast_text")
                hasMaiChatBroadcast = true;
            if (tool["function"]["name"] == "mobile_list_photos") hasMobilePhotos = true;
            if (tool["function"]["name"] == "mobile_list_albums") hasMobileAlbums = true;
            if (tool["function"]["name"] == "mobile_read_photo") hasMobilePhotoRead = true;
            if (tool["function"]["name"] == "mobile_export_photo_original")
                hasMobilePhotoOriginal = true;
            if (tool["function"]["name"] == "mobile_save_image") hasMobilePhotoSave = true;
            if (tool["function"]["name"] == "mobile_transform_image")
                hasMobileTransform = true;
            if (tool["function"]["name"] == "mobile_beautify_image")
                hasMobileBeautify = true;
            if (tool["function"]["name"] == "mobile_image_info") hasMobileImageInfo = true;
            if (tool["function"]["name"] == "mobile_preview_image") hasMobilePreview = true;
            if (kPhotoAlbumTool != nullptr && tool["function"]["name"] == kPhotoAlbumTool)
                hasMobilePhotoAlbumWrite = true;
        }
        CHECK(hasViewImage);
        CHECK(hasGeneratePdf);
        CHECK(hasMaiChatContacts);
        CHECK(hasMaiChatSend);
        CHECK(hasMaiChatBroadcast);
        CHECK(hasMobilePhotos);
        CHECK(hasMobileAlbums);
        CHECK(hasMobilePhotoRead);
        CHECK(hasMobilePhotoOriginal);
        CHECK(hasMobilePhotoSave);
        CHECK(hasMobileTransform);
        CHECK(hasMobileBeautify);
        CHECK(hasMobileImageInfo);
        CHECK(hasMobilePreview);
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
        if (input == "write") {
            Json invocation = {
                {"index", 0},
                {"id", "call_write"},
                {"type", "function"},
                {"function",
                 {{"name", "write"},
                  {"arguments", "{\"path\":\"result.txt\",\"content\":\"saved\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
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
#if !defined(_WIN32)
        pthread_setname_np("MaiMobileTestHTTP");
#endif
        server.listen_after_bind();
    });
    const MaiFilePath testRoot = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-mobile-test-" + MaiIdGenerator::newEventId()));
    CHECK(!MaiFileSystem::createDirectories(testRoot).hasError());
    const std::string directory = testRoot.toUtf8();
    void* agent = maiMobileAgentCreate();
    HostToolProbe hostProbe;
    int status = 0;
    try {
        CHECK(maiMobileAgentSetHostToolHandler(agent, &hostProbe, nullptr, nullptr, nullptr) == 0);
        CHECK(maiMobileAgentSetHostToolHandler(agent, &hostProbe, hostToolHandler,
                                               freeHostToolResponse,
                                               releaseHostToolContext) == 1);
        Json config = {
            {"op", "configure"},      {"baseUrl", "http://127.0.0.1:" + std::to_string(port)},
            {"apiKey", "test-key"},   {"model", "test"},
            {"workspace", directory}, {"database", directory + "/sessions.db"}};
        CHECK(call(agent, config)["ok"] == true);
        const Json suggestions = call(
            agent,
            {{"op", "suggest_replies"},
             {"messages", Json::array({{{"speaker", "friend"}, {"text", "今天能完成吗？"}}})}});
        CHECK(suggestions["natural"] == "好的");
        CHECK(suggestions["casual"] == "收到 😄");
        CHECK(suggestions["professional"] == "已收到，我会尽快处理。");
        std::string session = call(agent, {{"op", "create"}}).at("id");
        auto snapshot = [&] {
            return call(agent, {{"op", "snapshot"}, {"session", session}, {"force", true}});
        };
        auto wait = [&](const auto& condition) {
            for (int i = 0; i < 160; ++i) {
                auto s = snapshot();
                if (condition(s)) return s;
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            throw std::runtime_error("timed out waiting for native state");
        };
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "host-send"}})["ok"] ==
              true);
        const auto hostPermission =
            wait([](const Json& s) { return !s["permissions"].empty(); });
        CHECK(hostProbe.calls == 0);
        CHECK(hostPermission["permissions"][0]["tool"] == "maichat_send_text");
        CHECK(hostPermission["permissions"][0]["allowForSession"] == false);
        CHECK(call(agent,
                   {{"op", "permission"},
                    {"id", hostPermission["permissions"][0]["id"]},
                    {"decision", "approved"}})["ok"] == true);
        wait([](const Json& s) { return s["busy"] == false; });
        CHECK(hostProbe.calls == 1);
        CHECK(hostProbe.lastTool == "maichat_send_text");
        CHECK(Json::parse(hostProbe.lastArguments)["peer_id"] == "alice");
        if (kPhotoAlbumTool != nullptr) {
            CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-add"}})["ok"] == true);
            const auto photoDone = wait([](const Json& s) { return s["busy"] == false; });
            CHECK(photoDone["permissions"].empty());
            CHECK(hostProbe.calls == 2);
            CHECK(hostProbe.lastTool == kPhotoAlbumTool);
        }
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-save"}})["ok"] == true);
        const auto photoSaved = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoSaved["permissions"].empty());
        CHECK(hostProbe.calls == (kPhotoAlbumTool == nullptr ? 2 : 3));
        CHECK(hostProbe.lastTool == "mobile_save_image");
        CHECK(Json::parse(hostProbe.lastArguments)["path"] == "created.png");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-transform"}})["ok"] == true);
        const auto photoTransformed = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoTransformed["permissions"].empty());
        CHECK(hostProbe.calls == (kPhotoAlbumTool == nullptr ? 3 : 4));
        CHECK(hostProbe.lastTool == "mobile_transform_image");
        CHECK(Json::parse(hostProbe.lastArguments)["operation"] == "beautify");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-preview"}})["ok"] == true);
        const auto photoPreviewed = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoPreviewed["permissions"].empty());
        CHECK(hostProbe.calls == 5);
        CHECK(hostProbe.lastTool == "mobile_preview_image");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "hello"}})["ok"] == true);
        CHECK(call(agent, config)["ok"] == false);  // 工作中不能销毁并换配置。
        auto partial = wait([](const Json& s) {
            return s["busy"] == true && s.dump().find("hello ") != std::string::npos;
        });
        CHECK(partial.dump().find("reasoning") != std::string::npos);
        auto done = wait([](const Json& s) { return s["busy"] == false; });
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
        CHECK(call(agent, config)["ok"] == true);
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "relocated"}})["ok"] ==
              true);
        const auto relocated = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(relocated.value("error", "").empty());

        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "write"}})["ok"] == true);
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
