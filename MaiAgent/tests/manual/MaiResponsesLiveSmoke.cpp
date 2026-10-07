#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "MaiOpenAiClient.h"

int main(int argc, char** argv) {
    const bool withTool = argc == 2 && std::string(argv[1]) == "--tool-cycle";
    if (argc != 1 && !withTool) {
        std::cerr << "usage: MaiResponsesLiveSmoke [--tool-cycle]\n";
        return 2;
    }
    const char* key = std::getenv("MAIAGENT_DEEPSEEK_LIVE_KEY");
    if (key == nullptr || *key == '\0') {
        std::cerr << "MAIAGENT_DEEPSEEK_LIVE_KEY is required\n";
        return 2;
    }
    MaiModelConfig config;
    config.baseUrl = "https://api.deepseek.com";
    config.apiKey = key;
    config.wire = MaiWireApi::Responses;
    config.maxRetries = 0;
    config.totalTimeoutSeconds = 60;
    auto client = makeMaiModelClient(std::move(config));
    MaiModelRequest request;
    request.model = "deepseek-flash";
    request.baseInstructions = withTool ? "Call the echo function exactly once with text OK. "
                                          "After its result arrives, reply with exactly OK."
                                        : "Reply with exactly OK.";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = withTool ? "Use echo to say OK." : "ping";
    request.messages.push_back(std::move(user));
    if (withTool)
        request.tools.push_back(
            {"echo", "Return the provided text.",
             R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})"});
    std::string text;
    std::string reasoning;
    std::vector<MaiToolInvocation> calls;
    MaiStreamSink sink;
    sink.onText = [&text](std::string_view delta) { text.append(delta); };
    sink.onReasoning = [&reasoning](std::string_view delta) { reasoning.append(delta); };
    sink.onToolCall = [&calls](const MaiToolInvocation& call) { calls.push_back(call); };
    const std::atomic<bool> cancel{false};
    const MaiError error = client->stream(request, sink, cancel);
    if (error) {
        std::cerr << "request failed: " << error.message() << '\n';
        return 1;
    }
    if (withTool) {
        if (calls.size() != 1 || calls.front().name != "echo") {
            std::cerr << "expected one echo call; received " << calls.size() << '\n';
            return 1;
        }
        MaiModelMessage assistant;
        assistant.role = MaiModelRole::Assistant;
        assistant.reasoning = reasoning;
        assistant.content = text;
        assistant.invocations = calls;
        request.messages.push_back(std::move(assistant));
        MaiModelMessage output;
        output.role = MaiModelRole::ToolResult;
        output.toolCallId = calls.front().id;
        output.content = "OK";
        request.messages.push_back(std::move(output));
        text.clear();
        reasoning.clear();
        calls.clear();
        const MaiError followup = client->stream(request, sink, cancel);
        if (followup) {
            std::cerr << "follow-up failed: " << followup.message() << '\n';
            return 1;
        }
        if (!calls.empty()) {
            std::cerr << "unexpected repeated tool call\n";
            return 1;
        }
    }
    std::cout << "response: " << text << '\n';
    return text.empty() ? 1 : 0;
}
