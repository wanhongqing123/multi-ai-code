#include "MaiUploadFileTool.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <json.hpp>

#include "MaiCurlTools.h"
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

struct Server {
    httplib::Server server;
    std::thread worker;
    int port = 0;
    std::mutex mutex;
    std::string lastBody;
    std::string lastType;

    Server() {
        const auto receive = [this](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                lastBody = request.body;
                lastType = request.get_header_value("Content-Type");
            }
            response.status = 201;
            response.set_content("stored", "text/plain");
        };
        server.Put("/raw", receive);
        server.Post("/raw", receive);
        server.Put("/reject", [](const httplib::Request&, httplib::Response& response) {
            response.status = 403;
        });
        server.Put("/redirect", [](const httplib::Request&, httplib::Response& response) {
            response.set_redirect("/raw");
        });
        port = server.bind_to_any_port("127.0.0.1");
        worker = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ~Server() {
        server.stop();
        if (worker.joinable()) worker.join();
    }
    std::string url(const char* route) const {
        return "http://127.0.0.1:" + std::to_string(port) + route;
    }
};

}  // namespace

int main() {
    Server server;
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-upload-test-")));
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath source = root.append(MaiFilePath::fromUtf8("clip.bin"));
    const std::string bytes = std::string("abc\0", 4) + std::string(4096, 'x');
    CHECK(!MaiFileSystem::writeFile(source, bytes));
    MaiToolContext context;
    context.root = root.toUtf8();
    auto upload = makeMaiUploadFileTool();
    CHECK(upload->requiresPerCallApproval("{}"));
    MaiToolRegistry registry;
    registerMaiCurlTools(registry);
    CHECK(registry.find("curl") != nullptr);
    CHECK(registry.find("curl_fetch") == nullptr);
    CHECK(registry.find("curl_download") == nullptr);
    CHECK(registry.find("curl_request") == nullptr);
    CHECK(registry.find("curl_upload") == nullptr);

    for (const char* method : {"PUT", "POST"}) {
        const auto result = upload->execute(nlohmann::json{{"url", server.url("/raw")},
                                                           {"path", "clip.bin"},
                                                           {"method", method},
                                                           {"content_type", "video/mp4"}}
                                                .dump(),
                                            context);
        CHECK(!result.hasError());
        if (!result.hasError()) {
            const auto output = nlohmann::json::parse(result.output());
            CHECK(output.value("status", 0) == 201);
            CHECK(output.value("bytes", std::uint64_t{0}) == bytes.size());
            {
                std::lock_guard<std::mutex> lock(server.mutex);
                CHECK(server.lastBody == bytes);
                CHECK(server.lastType == "video/mp4");
            }
        }
    }
    CHECK(upload->execute(R"({"url":"file:///tmp/out","path":"clip.bin"})", context).hasError());
    CHECK(upload->execute(R"({"url":"http://user:pass@127.0.0.1/file","path":"clip.bin"})", context)
              .hasError());
    CHECK(upload
              ->execute(
                  nlohmann::json{{"url", server.url("/raw")}, {"path", "../outside.bin"}}.dump(),
                  context)
              .hasError());
    CHECK(upload
              ->execute(nlohmann::json{{"url", server.url("/reject")}, {"path", "clip.bin"}}.dump(),
                        context)
              .hasError());
    CHECK(
        upload
            ->execute(nlohmann::json{{"url", server.url("/redirect")}, {"path", "clip.bin"}}.dump(),
                      context)
            .hasError());
    MaiFileSystem::removeRecursively(root);
    return failures == 0 ? 0 : 1;
}
