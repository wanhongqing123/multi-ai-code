#include "MaiSshTool.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

}  // namespace

int main() {
    int passwordCalls = 0;
    int trustCalls = 0;
    auto tool = makeMaiSshTool(
        [&passwordCalls](const std::string&, int, const std::string&) -> MaiResult<std::string> {
            ++passwordCalls;
            return std::string("test-password");
        },
        [&trustCalls](const std::string&, int, const std::string&) -> MaiResult<bool> {
            ++trustCalls;
            return true;
        });
    CHECK(tool->requiresPerCallApproval("{}"));
    const auto schema = nlohmann::json::parse(tool->parametersSchema());
    CHECK(!schema["properties"].contains("password"));
    MaiToolContext context;
    CHECK(tool->execute("{}", context).hasError());
    CHECK(tool->execute(R"({"host":"bad/path","username":"test","command":"id"})", context)
              .hasError());
    CHECK(passwordCalls == 0);

    const char* localPort = std::getenv("MAI_SSH_TEST_PORT");
    if (localPort) {
        MaiSshExecRequest request;
        request.host = "127.0.0.1";
        request.port = std::atoi(localPort);
        request.username = "test";
        request.password = "test-password";
        request.command = "test-command";
        request.timeoutSeconds = 10;
        bool verified = false;
        const auto completed =
            maiExecuteSsh(request,
                          [&verified](const std::string&, int,
                                      const std::string& fingerprint) -> MaiResult<bool> {
                              verified = fingerprint.find("SHA256:") == 0;
                              return verified;
                          });
        CHECK(completed);
        CHECK(verified);
        if (completed) {
            CHECK(completed.value().stdoutText == "ssh-stdout");
            CHECK(completed.value().stderrText == "ssh-stderr");
            CHECK(completed.value().hasExitCode);
            CHECK(completed.value().exitCode == 7);
            CHECK(!completed.value().timedOut);
        }
        const auto rejected = maiExecuteSsh(
            request,
            [](const std::string&, int, const std::string&) -> MaiResult<bool> { return false; });
        CHECK(!rejected);

        context.sessionId = "session-one";
        const nlohmann::json firstArgs = {{"host", "127.0.0.1"},
                                          {"port", request.port},
                                          {"username", "test"},
                                          {"command", "test-command"}};
        const auto first = tool->execute(firstArgs.dump(), context);
        CHECK(!first.hasError());
        if (!first.hasError()) {
            const auto output = nlohmann::json::parse(first.output());
            CHECK(output.at("connection_reused") == false);
            CHECK(output.at("stdout") == "ssh-stdout");
            CHECK(output.at("exit_code") == 7);
        }
        auto secondArgs = firstArgs;
        secondArgs["command"] = "second-command";
        const auto second = tool->execute(secondArgs.dump(), context);
        CHECK(!second.hasError());
        if (!second.hasError()) {
            const auto output = nlohmann::json::parse(second.output());
            CHECK(output.at("connection_reused") == true);
            CHECK(output.at("stdout") == "ssh-second");
        }
        CHECK(passwordCalls == 1);
        CHECK(trustCalls == 1);
        context.sessionId = "session-two";
        const auto third = tool->execute(secondArgs.dump(), context);
        CHECK(!third.hasError());
        if (!third.hasError())
            CHECK(nlohmann::json::parse(third.output()).at("connection_reused") == false);
        CHECK(passwordCalls == 2);
        CHECK(trustCalls == 2);
    }
    return failures ? 1 : 0;
}
