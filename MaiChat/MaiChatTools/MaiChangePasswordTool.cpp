#include "MaiChangePasswordTool.h"

#include <string>
#include <utility>

#include <json.hpp>

namespace {

using Json = nlohmann::json;

bool isRequestId(const Json& value) {
    if (!value.is_string()) return false;
    const auto& id = value.get_ref<const std::string&>();
    if (id.size() != 36) return false;
    for (size_t index = 0; index < id.size(); ++index) {
        const char c = id[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

MaiToolResult invalidInput() {
    // 不回显未知键或原始 JSON；调用者可能误把密码填进参数。
    return MaiToolResult::failure(
        MaiErrorCode::InvalidInput,
        "Use open without request_id, or status with the returned request_id. "
        "Enter passwords only in the device's secure account form, never in chat or tool arguments.");
}

class MaiChangePasswordTool final : public MaiTool {
public:
    explicit MaiChangePasswordTool(MaiChangePasswordToolHandler handler) : mHandler(std::move(handler)) {}

    std::string name() const override {
        return "maichat_change_password";
    }
    std::string description() const override {
        return "Open MaiChat's secure login-password form, or query its request_id later. "
               "Never ask for or pass a password in chat or tool arguments. The user enters the "
               "new password directly on the device. opened/awaiting_user is not changed; only "
               "changed confirms the server saved it. Recent sign-in is required. Changing the "
               "password signs other devices out, while keeping this device signed in.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["open","status"]},)"
               R"("request_id":{"type":"string","format":"uuid","minLength":36,"maxLength":36}},)"
               R"("required":["action"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& argumentsJson, const MaiToolContext& context) override {
        Json args = Json::parse(argumentsJson, nullptr, false);
        if (!args.is_object() || !args.contains("action") || !args["action"].is_string()) return invalidInput();
        for (const auto& field : args.items()) {
            if (field.key() != "action" && field.key() != "request_id") return invalidInput();
        }
        const std::string action = args["action"].get<std::string>();
        if ((action != "open" && action != "status") || (action == "open" && args.contains("request_id")) ||
            (action == "status" && (!args.contains("request_id") || !isRequestId(args["request_id"]))))
            return invalidInput();
        if (action == "status") {
            // UUID 大小写不影响身份，统一小写避免不同平台规范化后的状态关联误报。
            std::string id = args["request_id"].get<std::string>();
            for (char& c : id) {
                if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
            }
            args["request_id"] = id;
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "password change request was canceled");
        if (!mHandler)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "secure password form is unavailable");
        const MaiToolResult result = mHandler(args.dump(), context);
        // 普通表单/HTTP 失败可能携带底层文本；模型仅接收固定文案，不能回传请求体或密码。
        if (result.hasError())
            return MaiToolResult::failure(result.error().code(), "secure password form request failed");
        const Json output = Json::parse(result.output(), nullptr, false);
        if (!output.is_object() || !output.contains("request_id") || !isRequestId(output["request_id"]) ||
            !output.contains("opened") || !output["opened"].is_boolean() || !output.contains("changed") ||
            !output["changed"].is_boolean() || !output.contains("status") || !output["status"].is_string())
            return protocolFailure();
        for (const auto& field : output.items()) {
            if (field.key() != "request_id" && field.key() != "opened" && field.key() != "changed" &&
                field.key() != "status" && field.key() != "error_code" && field.key() != "message")
                return protocolFailure();
        }
        const std::string status = output["status"].get<std::string>();
        const bool opened = output["opened"].get<bool>();
        const bool changed = output["changed"].get<bool>();
        if ((status != "opening" && status != "awaiting_user" && status != "submitting" && status != "changed" &&
             status != "canceled" && status != "failed" && status != "reauth_required") ||
            changed != (status == "changed") || (changed && !opened) || (status == "opening" && opened) ||
            ((status == "awaiting_user" || status == "submitting") && !opened) ||
            (action == "status" && output["request_id"] != args["request_id"]))
            return protocolFailure();
        Json safe = {
            {"request_id", output["request_id"]}, {"opened", opened}, {"status", status}, {"changed", changed}};
        if (output.contains("error_code")) {
            if (!output["error_code"].is_string()) return protocolFailure();
            const std::string code = output["error_code"].get<std::string>();
            if (code != "reauth_required" && code != "network" && code != "invalid_input" &&
                code != "identity_changed" && code != "unavailable" && code != "canceled" && code != "failed")
                return protocolFailure();
            safe["error_code"] = code;
        }
        safe["message"] = changed                       ? "The server confirmed the password was changed."
                          : status == "reauth_required" ? "Sign in again, then reopen the secure password form."
                          : status == "canceled"        ? "The password-change form was canceled."
                          : status == "failed" ? "Password change was not confirmed; use the secure form to retry."
                          : status == "submitting"
                              ? "The password change is being submitted; completion is not confirmed."
                              : "The password has not changed. Complete the secure password form on the device.";
        return MaiToolResult::success(safe.dump());
    }

private:
    static MaiToolResult protocolFailure() {
        return MaiToolResult::failure(MaiErrorCode::Protocol, "invalid secure password form status");
    }
    MaiChangePasswordToolHandler mHandler;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiChangePasswordTool(MaiChangePasswordToolHandler handler) {
    return std::make_unique<MaiChangePasswordTool>(std::move(handler));
}
