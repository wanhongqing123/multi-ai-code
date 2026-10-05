#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiCurlCliTool.h"
#include "MaiTool.h"
#include "mai_curl_embed.h"

namespace {

int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

int run(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    return mai_curl_execute(static_cast<int>(arguments.size()), argv.data());
}

int cancelImmediately(void*) {
    return 1;
}

struct Server {
    httplib::Server server;
    std::thread worker;
    int port = 0;

    Server() {
        server.Get("/ok", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("embedded curl", "text/plain");
        });
        server.Get("/html", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("<p>Readable <b>page</b></p><script>bad()</script>", "text/html");
        });
        server.Get("/binary", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(std::string(1, static_cast<char>(0xff)),
                                 "application/octet-stream");
        });
        server.Get("/missing", [](const httplib::Request&, httplib::Response& response) {
            response.status = 404;
        });
        server.Post("/echo", [](const httplib::Request& request, httplib::Response& response) {
            response.set_content(request.body, "text/plain");
        });
        server.Put("/upload", [](const httplib::Request& request, httplib::Response& response) {
            response.set_content(std::to_string(request.body.size()), "text/plain");
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
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("curl-cli-test-")));
    CHECK(!MaiFileSystem::createDirectories(root));
    const auto invoke = [&](const char* route, const char* output) {
        const MaiFilePath path = root.append(MaiFilePath::fromUtf8(output));
        const int status =
            run({"curl", "--silent", "--show-error", "--fail", "--proto", "=http,https",
                 "--max-time", "5", "--output", path.toUtf8(), server.url(route)});
        return std::pair<int, MaiFilePath>{status, path};
    };
    const auto first = invoke("/ok", "first.txt");
    CHECK(first.first == 0);
    std::string content;
    CHECK(!MaiFileSystem::readFile(first.second, content));
    CHECK(content == "embedded curl");

    MaiToolContext context;
    context.root = root.toUtf8();
    auto tool = makeMaiCurlCliTool();
    CHECK(tool->requiresApproval("{}"));
    CHECK(!tool->requiresPerCallApproval("{}"));
    CHECK(tool->approvalKey(R"({"arguments":["https://a.example"]})") !=
          tool->approvalKey(R"({"arguments":["https://b.example"]})"));
    const auto fetched =
        tool->execute(nlohmann::json{{"arguments", {server.url("/ok")}}}.dump(), context);
    CHECK(!fetched.hasError());
    if (!fetched.hasError()) {
        const auto result = nlohmann::json::parse(fetched.output());
        CHECK(result.value("status", 0) == 200);
        CHECK(result.value("body", std::string{}) == "embedded curl");
        CHECK(result["headers"].value("content-type", std::string{}) == "text/plain");
    }
    const auto html = tool->execute(
        nlohmann::json{{"arguments", {server.url("/html")}}, {"text_only", true}}.dump(), context);
    CHECK(!html.hasError());
    if (!html.hasError()) {
        const auto text = nlohmann::json::parse(html.output()).value("body", std::string{});
        CHECK(text.find("Readable page") != std::string::npos);
        CHECK(text.find("bad()") == std::string::npos);
    }
    const auto binary =
        tool->execute(nlohmann::json{{"arguments", {server.url("/binary")}}}.dump(), context);
    CHECK(!binary.hasError());
    if (!binary.hasError())
        CHECK(nlohmann::json::parse(binary.output()).value("body_base64", std::string{}) == "/w==");
    const auto saved = tool->execute(
        nlohmann::json{{"arguments", {server.url("/ok")}}, {"output_path", "saved.txt"}}.dump(),
        context);
    CHECK(!saved.hasError());
    CHECK(MaiFileSystem::exists(root.append(MaiFilePath::fromUtf8("saved.txt"))));
    CHECK(tool->execute(
                  nlohmann::json{{"arguments", {server.url("/ok")}}, {"output_path", "saved.txt"}}
                      .dump(),
                  context)
              .hasError());
    const auto posted = tool->execute(
        nlohmann::json{{"arguments", {"--data", "hello", server.url("/echo")}}}.dump(), context);
    CHECK(!posted.hasError());
    if (!posted.hasError())
        CHECK(nlohmann::json::parse(posted.output()).value("body", std::string{}) == "hello");
    const MaiFilePath upload = root.append(MaiFilePath::fromUtf8("input.bin"));
    CHECK(!MaiFileSystem::writeFile(upload, std::string(4096, 'u')));
    const auto uploaded = tool->execute(
        nlohmann::json{{"arguments", {"--upload-file", "input.bin", server.url("/upload")}}}.dump(),
        context);
    CHECK(!uploaded.hasError());
    if (!uploaded.hasError())
        CHECK(nlohmann::json::parse(uploaded.output()).value("body", std::string{}) == "4096");
    const auto missing =
        tool->execute(nlohmann::json{{"arguments", {server.url("/missing")}}}.dump(), context);
    CHECK(!missing.hasError());
    if (!missing.hasError())
        CHECK(nlohmann::json::parse(missing.output()).value("status", 0) == 404);
    CHECK(tool->execute(nlohmann::json{{"arguments", {server.url("/missing")}},
                                       {"output_path", "missing.bin"}}
                            .dump(),
                        context)
              .hasError());
    CHECK(!MaiFileSystem::exists(root.append(MaiFilePath::fromUtf8("missing.bin"))));
    CHECK(
        tool->execute(nlohmann::json{{"arguments", {"--config", "bad", server.url("/ok")}}}.dump(),
                      context)
            .hasError());
    CHECK(tool->execute(nlohmann::json{{"arguments",
                                        {"--upload-file", "../outside", server.url("/upload")}}}
                            .dump(),
                        context)
              .hasError());
    FILE* errors = std::tmpfile();
    CHECK(errors != nullptr);
    if (errors) mai_curl_set_error_file(errors);
    const auto failed = invoke("/missing", "failed.txt");
    mai_curl_set_error_file(nullptr);
    CHECK(failed.first != 0);
    if (errors) {
        std::rewind(errors);
        char text[256]{};
        const std::size_t count = std::fread(text, 1, sizeof(text) - 1, errors);
        CHECK(std::string(text, count).find("404") != std::string::npos);
        std::fclose(errors);
    }
    mai_curl_set_cancel_check(cancelImmediately, nullptr);
    const auto canceled = invoke("/ok", "canceled.txt");
    mai_curl_set_cancel_check(nullptr, nullptr);
    CHECK(canceled.first != 0);
    const auto second = invoke("/ok", "second.txt");
    CHECK(second.first == 0);
    content.clear();
    CHECK(!MaiFileSystem::readFile(second.second, content));
    CHECK(content == "embedded curl");
    MaiFileSystem::removeRecursively(root);
    return failures == 0 ? 0 : 1;
}
