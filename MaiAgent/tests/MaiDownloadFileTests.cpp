#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <httplib.h>
#include <json.hpp>

#include "MaiDownloadFileTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"
#include "MaiTool.h"

using nlohmann::json;

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

struct FakeServer {
    httplib::Server server;
    std::thread thread;
    int port = 0;
    std::string bytes = std::string("MP3\0", 4) + std::string(1, static_cast<char>(0xff));

    FakeServer() {
        server.Get("/binary", [this](const httplib::Request&, httplib::Response& response) {
            response.set_header("Content-Disposition", "attachment; filename*=UTF-8''song.mp3");
            response.set_content(bytes, "audio/mpeg");
        });
        server.Get("/redirect", [](const httplib::Request&, httplib::Response& response) {
            response.set_redirect("/binary");
        });
        server.Get("/large", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(std::string(2 * 1024 * 1024, 'x'), "application/octet-stream");
        });
        server.Get("/missing", [](const httplib::Request&, httplib::Response& response) {
            response.status = 404;
        });
        server.Get("/slow", [](const httplib::Request&, httplib::Response& response) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            response.set_content("late", "text/plain");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int index = 0; index < 200 && !server.is_running(); ++index)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~FakeServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

MaiFilePath testWorkspace() {
    const MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai_download_test_")));
    CHECK(!MaiFileSystem::createDirectories(path));
    return path;
}

void testBinaryRedirectAndFilename() {
    FakeServer server;
    const MaiFilePath root = testWorkspace();
    MaiToolContext context;
    context.root = root.toUtf8();
    auto tool = makeMaiDownloadFileTool();
    CHECK(tool->requiresApproval("{}"));
    CHECK(tool->approvalKey(json{{"url", server.url("/binary")}}.dump()) ==
          "download_file:127.0.0.1");
    const MaiToolResult result =
        tool->execute(json{{"url", server.url("/redirect")}}.dump(), context);
    CHECK(!result.hasError());
    if (!result.hasError()) {
        const json output = json::parse(result.output());
        CHECK(output.value("bytes", 0) == 5);
        CHECK(output.value("mime_type", "") == "audio/mpeg");
        CHECK(output.value("path", "") == maiResolvePathWithinRoot(context.root, "song.mp3"));
        std::string contents;
        CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(output["path"].get<std::string>()),
                                       contents));
        CHECK(contents == server.bytes);
    }
    const MaiToolResult duplicate =
        tool->execute(json{{"url", server.url("/binary")}}.dump(), context);
    CHECK(duplicate.hasError());
    if (duplicate.hasError())
        CHECK(json::parse(duplicate.error().message()).value("code", "") == "file_exists");
    MaiFileSystem::removeRecursively(root);
}

void testPathSizeHttpAndTimeoutFailures() {
    FakeServer server;
    const MaiFilePath root = testWorkspace();
    MaiToolContext context;
    context.root = root.toUtf8();
    auto tool = makeMaiDownloadFileTool();
    const auto checkCode = [&](const json& arguments, const char* code) {
        const MaiToolResult result = tool->execute(arguments.dump(), context);
        CHECK(result.hasError());
        if (result.hasError())
            CHECK(json::parse(result.error().message()).value("code", "") == code);
    };
    checkCode({{"url", "file:///etc/passwd"}}, "invalid_url");
    checkCode({{"url", server.url("/binary")}, {"output_path", 42}}, "invalid_path");
    checkCode({{"url", server.url("/binary")}, {"output_path", "../outside.mp3"}}, "invalid_path");
    checkCode({{"url", server.url("/large")}, {"max_size_mb", 1}}, "too_large");
    checkCode({{"url", server.url("/missing")}}, "http_error");
    checkCode({{"url", server.url("/slow")}, {"timeout_s", 1}}, "timeout");
    MaiFileSystem::removeRecursively(root);
}

}  // namespace

int main() {
    testBinaryRedirectAndFilename();
    testPathSizeHttpAndTimeoutFailures();
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    CHECK(registry.find("download_file") != nullptr);
    return failures == 0 ? 0 : 1;
}
