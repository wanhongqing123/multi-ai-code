#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMobileAgent.h"
#include "MaiSqliteStore.h"
#include <chrono>
#include <cstdio>
#include <httplib.h>
#include <json.hpp>
#include <stdexcept>
#include <thread>
#include <unistd.h>
using Json = nlohmann::json;
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
static std::string frame(const Json& delta, const char* finish = nullptr) {
    Json choice = {
        {"delta", delta}, {"index", 0}, {"finish_reason", finish ? Json(finish) : Json(nullptr)}};
    return "data: " + Json{{"choices", Json::array({choice})}}.dump() + "\n\n";
}
int main() {
    httplib::Server server;
    server.Post("/chat/completions", [](const httplib::Request& request,
                                        httplib::Response& response) {
        const Json body = Json::parse(request.body);
        CHECK(request.get_header_value("Authorization") == "Bearer test-key");
        CHECK(body["messages"].front()["role"] == "system");
        CHECK(body["messages"].front()["content"].get<std::string>().find(
                  "GitHub-Flavored Markdown") != std::string::npos);
        // 移动端不能向模型宣称可以执行桌面 shell。
        for (const auto& tool : body["tools"]) CHECK(tool["function"]["name"] != "shell");
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
        pthread_setname_np("MaiMobileTestHTTP");
        server.listen_after_bind();
    });
    char directory[] = "/tmp/mai-mobile-test-XXXXXX";
    CHECK(mkdtemp(directory));
    void* agent = maiMobileAgentCreate();
    int status = 0;
    try {
        Json config = {
            {"op", "configure"},      {"baseUrl", "http://127.0.0.1:" + std::to_string(port)},
            {"apiKey", "test-key"},   {"model", "test"},
            {"workspace", directory}, {"database", std::string(directory) + "/sessions.db"}};
        CHECK(call(agent, config)["ok"] == true);
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
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "write"}})["ok"] == true);
        auto pending = wait([](const Json& s) { return !s["permissions"].empty(); });
        std::string permission = pending["permissions"][0]["id"];
        CHECK(access((std::string(directory) + "/result.txt").c_str(), F_OK) != 0);
        CHECK(call(agent,
                   {{"op", "permission"}, {"id", permission}, {"decision", "unknown"}})["ok"] ==
              false);
        CHECK(snapshot()["permissions"].size() == 1);
        CHECK(call(agent,
                   {{"op", "permission"}, {"id", permission}, {"decision", "approved"}})["ok"] ==
              true);
        wait([](const Json& s) { return s["busy"] == false; });
        CHECK(access((std::string(directory) + "/result.txt").c_str(), F_OK) == 0);
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
    server.stop();
    listener.join();
    for (const auto* file : {"sessions.db", "sessions.db-shm", "sessions.db-wal", "result.txt"})
        unlink((std::string(directory) + "/" + file).c_str());
    rmdir(directory);
    return status;
}
