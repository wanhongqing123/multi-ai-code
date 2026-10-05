#include "MaiHttpRequestTool.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <httplib.h>
#include <json.hpp>

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
    std::thread thread;
    int port = 0;

    Server() {
        server.Get("/info", [](const httplib::Request&, httplib::Response& response) {
            response.set_header("ETag", "example");
            response.set_content("ready", "text/plain");
        });
        server.Post("/echo", [](const httplib::Request& request, httplib::Response& response) {
            response.status = 201;
            response.set_content(request.body, "text/plain");
        });
        server.Get("/redirect", [](const httplib::Request&, httplib::Response& response) {
            response.set_redirect("/info");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int index = 0; index < 200 && !server.is_running(); ++index)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~Server() {
        server.stop();
        thread.join();
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

}  // namespace

int main() {
    Server server;
    MaiToolContext context;
    auto tool = makeMaiHttpRequestTool();
    CHECK(tool->requiresPerCallApproval("{}"));
    CHECK(tool->execute(R"({"url":"file:///etc/passwd"})", context).hasError());
    CHECK(tool->execute(R"({"url":"http://a","headers":{"X-Test":"bad\r\nheader"}})", context)
              .hasError());
    const auto info = tool->execute(nlohmann::json{{"url", server.url("/info")}}.dump(), context);
    CHECK(!info.hasError());
    if (!info.hasError()) {
        const auto output = nlohmann::json::parse(info.output());
        CHECK(output.value("status", 0) == 200);
        CHECK(output.value("body", std::string{}) == "ready");
        CHECK(output["headers"].value("etag", std::string{}) == "example");
    }
    const auto post = tool->execute(
        nlohmann::json{{"url", server.url("/echo")}, {"method", "POST"}, {"body", "hi"}}.dump(),
        context);
    CHECK(!post.hasError());
    if (!post.hasError()) {
        const auto output = nlohmann::json::parse(post.output());
        CHECK(output.value("status", 0) == 201);
        CHECK(output.value("body", std::string{}) == "hi");
    }
    const auto redirect =
        tool->execute(nlohmann::json{{"url", server.url("/redirect")}}.dump(), context);
    CHECK(!redirect.hasError());
    if (!redirect.hasError())
        CHECK(nlohmann::json::parse(redirect.output()).value("status", 0) == 302);
    return failures ? 1 : 0;
}
