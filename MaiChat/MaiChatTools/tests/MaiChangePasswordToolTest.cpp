#include "MaiChangePasswordTool.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>

#include <json.hpp>

namespace {

using Json = nlohmann::json;
constexpr const char* kId = "123e4567-e89b-42d3-a456-426614174000";

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    int calls = 0;
    std::string arguments;
    Json response = {{"request_id", kId}, {"status", "opening"}, {"opened", false}, {"changed", false}};
    bool hostError = false;
    auto tool = makeMaiChangePasswordTool([&](const std::string& input, const MaiToolContext&) {
        ++calls;
        arguments = input;
        return hostError ? MaiToolResult::failure(MaiErrorCode::Network, "raw-secret-must-not-leak")
                         : MaiToolResult::success(response.dump());
    });
    MaiToolContext context;
    require(tool->name() == "maichat_change_password", "password tool name changed");
    const Json schema = Json::parse(tool->parametersSchema());
    require(schema["additionalProperties"] == false && schema["properties"].size() == 2 &&
                !schema["properties"].contains("password"),
            "password tool schema exposes credential input");
    require(!tool->requiresApproval(R"({"action":"open"})") && !tool->requiresPerCallApproval(R"({"action":"open"})") &&
                !tool->requiresPaidApproval(R"({"action":"open"})"),
            "opening the secure form added an extra approval");
    for (const char* input :
         {"{}", "[]", "invalid", R"({"action":"submit"})", R"({"action":"open","password":"raw-secret-must-not-leak"})",
          R"({"action":"open","request_id":"123e4567-e89b-42d3-a456-426614174000"})", R"({"action":"status"})",
          R"({"action":"status","request_id":3})", R"({"action":"status","request_id":"not-a-uuid"})"}) {
        const auto result = tool->execute(input, context);
        require(result.hasError() && calls == 0, "invalid/password-bearing input reached the host");
        require(result.error().message().find("raw-secret") == std::string::npos,
                "input validation echoed a credential");
    }
    require(!tool->execute(R"({"action":"open"})", context).hasError(), "opening state failed");
    require(Json::parse(arguments).size() == 1, "open added a password or request id");
    response["opened"] = true;
    for (const char* state : {"awaiting_user", "submitting", "changed", "canceled", "failed", "reauth_required"}) {
        response["status"] = state;
        response["changed"] = std::string(state) == "changed";
        // 宿主文本不能成为泄漏口；输出只保留共享实现的固定安全文案和状态。
        response["message"] = "raw-secret-must-not-leak";
        const auto result =
            tool->execute(R"({"action":"status","request_id":"123E4567-E89B-42D3-A456-426614174000"})", context);
        require(!result.hasError(), "valid form lifecycle status failed");
        require(result.output().find("raw-secret") == std::string::npos, "host message leaked into model history");
        require(Json::parse(arguments)["request_id"] == kId, "request UUID was not normalized");
        require(Json::parse(result.output())["changed"] == (std::string(state) == "changed"),
                "opening/awaiting/submitting was confused with changed");
    }
    response["status"] = "awaiting_user";
    response["changed"] = true;
    require(tool->execute(R"({"action":"open"})", context).hasError(),
            "unconfirmed awaiting status claimed password changed");
    response["changed"] = false;
    response["password"] = "raw-secret-must-not-leak";
    auto result = tool->execute(R"({"action":"open"})", context);
    require(result.hasError() && result.error().message().find("raw-secret") == std::string::npos,
            "credential-bearing output was returned to the model");
    response.erase("password");
    response["request_id"] = "223e4567-e89b-42d3-a456-426614174000";
    require(
        tool->execute(R"({"action":"status","request_id":"123e4567-e89b-42d3-a456-426614174000"})", context).hasError(),
        "status response crossed request identity");
    hostError = true;
    result = tool->execute(R"({"action":"open"})", context);
    require(result.hasError() && result.error().message().find("raw-secret") == std::string::npos,
            "raw authentication error reached the model");
    const int beforeCancel = calls;
    std::atomic<bool> cancel{true};
    context.cancel = &cancel;
    require(tool->execute(R"({"action":"open"})", context).hasError() && calls == beforeCancel,
            "canceled tool still opened a form");
    std::cout << "password tool contract passed\n";
    return 0;
}
