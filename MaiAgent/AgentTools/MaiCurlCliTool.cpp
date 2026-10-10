#include "MaiCurlCliTool.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiWebFetchTool.h"
#include "mai_curl_embed.h"

namespace {

using Json = nlohmann::json;
constexpr std::uint64_t kMegabyte = 1024 * 1024;
constexpr std::uint64_t kMaximumTextBytes = 256 * 1024;
constexpr std::uint64_t kMaximumHeaderBytes = 64 * 1024;
constexpr std::size_t kMaximumLogBytes = 16 * 1024;
std::mutex sCurlCommandMutex;

struct TemporaryFiles {
    std::vector<MaiFilePath> paths;
    ~TemporaryFiles() {
        for (const MaiFilePath& path : paths) MaiFileSystem::removeFile(path);
    }
};

bool isHttpUrl(const std::string& text) {
    if (text.empty() || text.size() > 8192 || text.find_first_of("{}[]") != std::string::npos)
        return false;
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, text.c_str(), 0) == CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK && host &&
                       *host && (std::string(scheme) == "http" || std::string(scheme) == "https") &&
                       curl_url_get(parsed, CURLUPART_USER, &user, 0) != CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK;
    if (scheme) curl_free(scheme);
    if (host) curl_free(host);
    if (user) curl_free(user);
    if (password) curl_free(password);
    curl_url_cleanup(parsed);
    return valid;
}

bool validHeader(const std::string& header) {
    if (header.empty() || header.size() > 4096 || header.find(':') == std::string::npos)
        return false;
    return std::none_of(header.begin(), header.end(),
                        [](unsigned char ch) { return ch == '\r' || ch == '\n' || ch == 0; });
}

int checkCanceled(void* opaque) {
    const auto* context = static_cast<const MaiToolContext*>(opaque);
    return context && context->isCanceled() ? 1 : 0;
}

std::string readLog(FILE* file, bool& truncated) {
    if (!file) return {};
    std::rewind(file);
    std::string result(kMaximumLogBytes + 1, '\0');
    const std::size_t count = std::fread(result.data(), 1, result.size(), file);
    truncated = count > kMaximumLogBytes;
    result.resize(std::min(count, kMaximumLogBytes));
    return result;
}

struct ResponseHeaders {
    int status = 0;
    Json fields = Json::object();
};

ResponseHeaders parseResponseHeaders(const std::string& headers) {
    ResponseHeaders response;
    std::size_t offset = 0;
    while (offset < headers.size()) {
        const std::size_t end = headers.find('\n', offset);
        const std::string line = headers.substr(offset, end - offset);
        if (line.rfind("HTTP/", 0) == 0) {
            response.fields = Json::object();
            const std::size_t separator = line.find(' ');
            if (separator != std::string::npos && separator + 4 <= line.size() &&
                std::isdigit(static_cast<unsigned char>(line[separator + 1])) &&
                std::isdigit(static_cast<unsigned char>(line[separator + 2])) &&
                std::isdigit(static_cast<unsigned char>(line[separator + 3])))
                response.status = (line[separator + 1] - '0') * 100 +
                                  (line[separator + 2] - '0') * 10 + (line[separator + 3] - '0');
        } else {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos && colon < 128) {
                std::string key = line.substr(0, colon);
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                if (key == "content-type" || key == "content-length" || key == "location" ||
                    key == "etag" || key == "last-modified" || key == "cache-control") {
                    const std::size_t first = line.find_first_not_of(" \t", colon + 1);
                    if (first != std::string::npos) {
                        std::string value = line.substr(first);
                        while (!value.empty() && (value.back() == '\r' || value.back() == ' ' ||
                                                  value.back() == '\t'))
                            value.pop_back();
                        response.fields[key] = value;
                    }
                }
            }
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return response;
}

std::string encodeBase64(const std::string& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < input.size(); index += 3) {
        const auto first = static_cast<unsigned char>(input[index]);
        const auto second =
            index + 1 < input.size() ? static_cast<unsigned char>(input[index + 1]) : 0;
        const auto third =
            index + 2 < input.size() ? static_cast<unsigned char>(input[index + 2]) : 0;
        output.push_back(alphabet[first >> 2]);
        output.push_back(alphabet[((first & 3) << 4) | (second >> 4)]);
        output.push_back(index + 1 < input.size() ? alphabet[((second & 15) << 2) | (third >> 6)]
                                                  : '=');
        output.push_back(index + 2 < input.size() ? alphabet[third & 63] : '=');
    }
    return output;
}

class CurlCliTool final : public MaiTool {
public:
    explicit CurlCliTool(std::string caBundlePath) : mCaBundlePath(std::move(caBundlePath)) {}

    std::string name() const override {
        return "curl";
    }
    std::string description() const override {
        // 这是受限的进程内 curl，不是任意命令行：只放行已列明的 HTTP 参数，
        // 上传文件必须来自工作区；输出和日志都有上限。
        return "Run the vendored curl command inside MaiChat without a subprocess. Supported "
               "arguments: one HTTP(S) URL, -X/--request, -H/--header, -d/--data (literal text), "
               "-T/--upload-file (workspace file), -I/--head and --compressed. Other CLI "
               "options are rejected. Use output_path for a new downloaded file; otherwise "
               "a bounded response body is returned. Set text_only=true to extract readable "
               "text from HTML. Approval follows the Agent policy; full access does not prompt.";
    }
    std::string parametersSchema() const override {
        // arguments 是受限 curl 参数列表；output_path 指向新下载文件，text_only
        // 控制 HTML 文本化；max_size_mb 和 timeout_s 分别限制返回体及等待时间。
        return R"({"type":"object","properties":{"arguments":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":32},"output_path":{"type":"string"},"text_only":{"type":"boolean"},"max_size_mb":{"type":"integer","minimum":1,"maximum":1024},"timeout_s":{"type":"integer","minimum":1,"maximum":600}},"required":["arguments"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return false;
    }

    std::string approvalKey(const std::string& raw) const override {
        const Json arguments = Json::parse(raw, nullptr, false);
        return "curl:" + (arguments.is_discarded() ? raw : arguments.dump());
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json request = Json::parse(raw, nullptr, false);
        if (!request.is_object() || !request.value("arguments", Json{}).is_array() ||
            request["arguments"].empty() || request["arguments"].size() > 32 ||
            (request.contains("output_path") && !request["output_path"].is_string()) ||
            (request.contains("text_only") && !request["text_only"].is_boolean()) ||
            (request.contains("max_size_mb") && !request["max_size_mb"].is_number_integer()) ||
            (request.contains("timeout_s") && !request["timeout_s"].is_number_integer()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid curl arguments");
        if (context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "Agent workspace is unavailable");
        const int maximumMb = request.value("max_size_mb", 100);
        const int timeout = request.value("timeout_s", 60);
        if (maximumMb < 1 || maximumMb > 1024 || timeout < 1 || timeout > 600)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "curl size or timeout is out of range");

        std::vector<std::string> options;
        std::string url;
        std::string method;
        bool hasData = false;
        bool hasUpload = false;
        const Json& arguments = request["arguments"];
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (!arguments[index].is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "every curl argument must be a string");
            const std::string option = arguments[index].get<std::string>();
            if (option.empty() || option.size() > 8192 || option.find('\0') != std::string::npos)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid curl argument");
            if (option == "--url" || option == "-X" || option == "--request" || option == "-H" ||
                option == "--header" || option == "-d" || option == "--data" || option == "-T" ||
                option == "--upload-file") {
                if (++index >= arguments.size() || !arguments[index].is_string())
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "curl option needs one string value");
                const std::string value = arguments[index].get<std::string>();
                if (value.empty() || value.find('\0') != std::string::npos)
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "curl option value is empty");
                if (option == "--url") {
                    if (!url.empty())
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "only one URL is allowed");
                    url = value;
                } else if (option == "-X" || option == "--request") {
                    if (!method.empty() ||
                        (value != "GET" && value != "HEAD" && value != "POST" && value != "PUT" &&
                         value != "PATCH" && value != "DELETE"))
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "unsupported HTTP method");
                    method = value;
                    options.insert(options.end(), {"--request", value});
                } else if (option == "-H" || option == "--header") {
                    if (!validHeader(value))
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "invalid HTTP header");
                    options.insert(options.end(), {"--header", value});
                } else if (option == "-d" || option == "--data") {
                    if (hasData || value.size() > 256 * 1024)
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "literal request body is too large");
                    hasData = true;
                    options.insert(options.end(), {"--data-raw", value});
                } else {
                    if (hasUpload)
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "only one upload file is allowed");
                    const std::string source = context.resolvePath(value);
                    std::uint64_t size = 0;
                    if (source.empty() ||
                        !MaiFileSystem::fileSize(MaiFilePath::fromUtf8(source), size) ||
                        size == 0 || size > static_cast<std::uint64_t>(maximumMb) * kMegabyte)
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "upload file is inaccessible or too large");
                    hasUpload = true;
                    options.insert(options.end(), {"--upload-file", source});
                }
            } else if (option == "-I" || option == "--head" || option == "--compressed") {
                options.push_back(option == "-I" ? "--head" : option);
            } else if (option[0] != '-' && url.empty()) {
                url = option;
            } else {
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "unsupported curl CLI argument: " + option);
            }
        }
        if (!isHttpUrl(url) || (hasData && hasUpload) ||
            (method == "HEAD" && (hasData || hasUpload)))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "invalid URL or incompatible curl options");
        const std::string requested = request.value("output_path", std::string{});
        if (!requested.empty() && MaiFilePath::fromUtf8(requested).isAbsolute())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "output_path must be relative");

        const std::string id = MaiIdGenerator::generate("curl_");
        const MaiFilePath temporary =
            MaiFilePath::fromUtf8(context.resolvePath(".mai-curl-" + id + ".body"));
        const MaiFilePath headers =
            MaiFilePath::fromUtf8(context.resolvePath(".mai-curl-" + id + ".headers"));
        if (temporary.isEmpty() || headers.isEmpty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "workspace temporary path is inaccessible");
        TemporaryFiles cleanup{{temporary, headers}};
        if (MaiFileSystem::createEmptyFile(temporary) || MaiFileSystem::createEmptyFile(headers))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot create curl output files");
        const std::uint64_t maximumOutput = requested.empty()
                                                ? kMaximumTextBytes
                                                : static_cast<std::uint64_t>(maximumMb) * kMegabyte;
        std::vector<std::string> command = {"curl",
                                            "-q",
                                            "--silent",
                                            "--show-error",
                                            "--proto",
                                            "=http,https",
                                            "--proto-redir",
                                            "=http,https",
                                            "--max-time",
                                            std::to_string(timeout),
                                            "--connect-timeout",
                                            "10",
                                            "--max-filesize",
                                            std::to_string(maximumOutput),
                                            "--output",
                                            temporary.toUtf8(),
                                            "--dump-header",
                                            headers.toUtf8()};
        if (!mCaBundlePath.empty()) command.insert(command.end(), {"--cacert", mCaBundlePath});
        command.insert(command.end(), options.begin(), options.end());
        command.push_back(url);
        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (std::string& option : command) argv.push_back(option.data());
        argv.push_back(nullptr);

        std::unique_ptr<FILE, decltype(&std::fclose)> errors(std::tmpfile(), std::fclose);
        if (!errors)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot create curl diagnostic stream");
        maiAssertBlockingAllowed("curl");
        int exitStatus = 0;
        {
            std::lock_guard<std::mutex> lock(sCurlCommandMutex);
            mai_curl_set_cancel_check(checkCanceled, const_cast<MaiToolContext*>(&context));
            mai_curl_set_error_file(errors.get());
            exitStatus = mai_curl_execute(static_cast<int>(command.size()), argv.data());
            mai_curl_set_error_file(nullptr);
            mai_curl_set_cancel_check(nullptr, nullptr);
        }
        bool logTruncated = false;
        const std::string log = readLog(errors.get(), logTruncated);
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "curl was canceled");
        if (exitStatus != 0)
            return MaiToolResult::failure(
                MaiErrorCode::Network,
                Json{
                    {"exit_status", exitStatus}, {"log_tail", log}, {"log_truncated", logTruncated}}
                    .dump());

        std::string headerBytes;
        bool headerTruncated = false;
        const MaiError headerError =
            MaiFileSystem::readFile(headers, headerBytes, kMaximumHeaderBytes, &headerTruncated);
        const ResponseHeaders response =
            headerError ? ResponseHeaders{} : parseResponseHeaders(headerBytes);
        const int httpStatus = response.status;
        if (headerError || headerTruncated || httpStatus == 0)
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          "curl returned no complete HTTP status");
        std::uint64_t received = 0;
        if (!MaiFileSystem::fileSize(temporary, received) || received > maximumOutput)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "curl response exceeds the output limit");

        if (!requested.empty()) {
            if (httpStatus < 200 || httpStatus >= 300)
                return MaiToolResult::failure(
                    MaiErrorCode::Network,
                    "curl download returned HTTP " + std::to_string(httpStatus));
            const std::string output = context.resolvePath(requested);
            if (output.empty() || MaiFileSystem::exists(MaiFilePath::fromUtf8(output)))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "output_path is unavailable or already exists");
            const MaiFilePath destination = MaiFilePath::fromUtf8(output);
            const MaiError directoryError = MaiFileSystem::createDirectories(destination.dirName());
            if (directoryError)
                return MaiToolResult::failure(directoryError.code(), directoryError.message());
            const MaiError published = MaiFileSystem::publishNewFile(temporary, destination);
            if (published) return MaiToolResult::failure(published.code(), published.message());
            return MaiToolResult::success(Json{
                {"status", httpStatus},
                {"headers", response.fields},
                {"path", output},
                {"bytes", received},
                {"exit_status", exitStatus}}.dump());
        }
        std::string body;
        bool truncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(temporary, body, kMaximumTextBytes, &truncated);
        if (readError || truncated)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot read the complete curl response");
        Json result = {{"status", httpStatus},
                       {"headers", response.fields},
                       {"bytes", received},
                       {"exit_status", exitStatus}};
        if (request.value("text_only", false) &&
            response.fields.value("content-type", std::string{}).find("text/html") !=
                std::string::npos)
            body = maiHtmlToText(std::move(body));
        try {
            Json text = body;
            text.dump();
            result["body"] = std::move(text);
        } catch (const Json::type_error&) {
            result["body_base64"] = encodeBase64(body);
        }
        return MaiToolResult::success(result.dump());
    }

private:
    std::string mCaBundlePath;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiCurlCliTool(std::string caBundlePath) {
    return std::make_unique<CurlCliTool>(std::move(caBundlePath));
}
