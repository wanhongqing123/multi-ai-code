#include "MaiOpenSslCliTool.h"

#include <json.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "mai_openssl_embed.h"

namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaximumOutput = 64 * 1024;
std::mutex sOpenSslMutex;

struct Output {
    std::string standard;
    std::string error;
    bool truncated = false;
};

void collectOutput(void* opaque, const char* bytes, std::size_t length, int isError) {
    auto& output = *static_cast<Output*>(opaque);
    std::string& destination = isError ? output.error : output.standard;
    const std::size_t remaining = kMaximumOutput - destination.size();
    destination.append(bytes, std::min(length, remaining));
    output.truncated |= length > remaining;
}

MaiResult<std::string> inputPath(const MaiToolContext& context, const std::string& candidate,
                                 std::uint64_t maximumBytes) {
    const std::string resolved = context.resolvePath(candidate);
    if (resolved.empty()) return {MaiErrorCode::InvalidInput, "path is outside the workspace"};
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(resolved), size) || size > maximumBytes)
        return {MaiErrorCode::InvalidInput, "input file is missing or exceeds the size limit"};
    return resolved;
}

MaiResult<std::vector<std::string>> checkedArguments(const std::vector<std::string>& input,
                                                     const MaiToolContext& context) {
    if (input.empty() || input.size() > 12)
        return {MaiErrorCode::InvalidInput, "invalid arguments"};
    for (const std::string& item : input) {
        if (item.empty() || item.size() > 4096 || item.find('\0') != std::string::npos)
            return {MaiErrorCode::InvalidInput, "invalid argument value"};
    }
    std::vector<std::string> result = input;
    if (result[0] == "version") {
        if (result.size() != 1) return {MaiErrorCode::InvalidInput, "version accepts no options"};
        return result;
    }
    if (result[0] == "dgst") {
        if (result.size() != 3 ||
            (result[1] != "-sha256" && result[1] != "-sha512" && result[1] != "-sha3-256" &&
             result[1] != "-blake2b512" && result[1] != "-md5"))
            return {MaiErrorCode::InvalidInput, "dgst needs one supported digest and one file"};
        auto path = inputPath(context, result[2], 64 * 1024 * 1024);
        if (!path) return path.error();
        result[2] = path.value();
        return result;
    }
    if (result[0] == "x509") {
        if (result.size() < 3 || result[1] != "-in")
            return {MaiErrorCode::InvalidInput, "x509 needs -in and a certificate file"};
        auto path = inputPath(context, result[2], 1024 * 1024);
        if (!path) return path.error();
        result[2] = path.value();
        bool der = false;
        for (std::size_t index = 3; index < result.size(); ++index) {
            const std::string& option = result[index];
            if (option == "-inform" && index + 1 < result.size() && !der &&
                result[index + 1] == "DER") {
                der = true;
                ++index;
            } else if (option != "-subject" && option != "-issuer" && option != "-dates" &&
                       option != "-fingerprint" && option != "-text" && option != "-noout") {
                return {MaiErrorCode::InvalidInput, "unsupported x509 option"};
            }
        }
        result.push_back("-noout");
        return result;
    }
    return {MaiErrorCode::InvalidInput, "unsupported OpenSSL subcommand"};
}

class OpenSslCliTool final : public MaiTool {
public:
    std::string name() const override {
        return "openssl";
    }
    std::string description() const override {
        // 只开放只读的摘要与证书检查子命令，不把完整 OpenSSL CLI 暴露给模型。
        return "Run a safe, read-only subset of the embedded OpenSSL CLI: version; dgst "
               "with SHA-256, SHA-512, SHA3-256, BLAKE2b-512 or MD5; x509 certificate "
               "inspection. Pass arguments as an array without the program name. "
               "Inputs must be workspace files. No keys, signing, output files or network.";
    }
    std::string parametersSchema() const override {
        // arguments 是最多 12 项的受限 OpenSSL 参数；执行层还会拦截未开放的子命令。
        return R"({"type":"object","properties":{"arguments":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":12}},"required":["arguments"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json parsed = Json::parse(raw, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("arguments") || !parsed["arguments"].is_array())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "arguments array required");
        std::vector<std::string> arguments;
        for (const Json& item : parsed["arguments"]) {
            if (!item.is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "string arguments required");
            arguments.push_back(item.get<std::string>());
        }
        auto result = maiRunOpenSslCommand(arguments, context);
        if (!result) return MaiToolResult::failure(result.error().code(), result.error().message());
        const MaiOpenSslCommandResult& command = result.value();
        return MaiToolResult::success(Json{{"exit_code", command.exitCode},
                                           {"stdout", command.standardOutput},
                                           {"stderr", command.errorOutput},
                                           {"truncated", command.truncated}}
                                          .dump(-1, ' ', false, Json::error_handler_t::replace),
                                      command.truncated);
    }
};

}  // namespace

MaiResult<MaiOpenSslCommandResult> maiRunOpenSslCommand(const std::vector<std::string>& arguments,
                                                        const MaiToolContext& context) {
    auto checked = checkedArguments(arguments, context);
    if (!checked) return checked.error();
    if (context.isCanceled()) return {MaiErrorCode::Canceled, "command was canceled"};
    std::lock_guard<std::mutex> lock(sOpenSslMutex);
    if (context.isCanceled()) return {MaiErrorCode::Canceled, "command was canceled"};
    std::vector<std::string> values = {"openssl"};
    for (const auto& item : checked.value()) values.push_back(item);
    std::vector<char*> argv;
    argv.reserve(values.size() + 1);
    for (std::string& item : values) argv.push_back(item.data());
    argv.push_back(nullptr);
    Output output;
    mai_openssl_set_output_sink(collectOutput, &output);
    const int status = mai_openssl_execute(static_cast<int>(values.size()), argv.data());
    mai_openssl_set_output_sink(nullptr, nullptr);
    return MaiOpenSslCommandResult{status, std::move(output.standard), std::move(output.error),
                                   output.truncated};
}

std::unique_ptr<MaiTool> makeMaiOpenSslCliTool() {
    return std::make_unique<OpenSslCliTool>();
}
