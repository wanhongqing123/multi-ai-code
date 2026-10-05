#include "MaiHttpRequestTool.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <string>

#include "MaiBlockingCheck.h"

namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumRequestBytes = 256 * 1024;
constexpr std::size_t kMaximumResponseBytes = 256 * 1024;

struct Response {
    std::string body;
    Json headers = Json::object();
    bool truncated = false;
    const MaiToolContext* context = nullptr;
};

bool isHttpUrl(const std::string& value) {
    CURLU* url = curl_url();
    if (!url) return false;
    const CURLUcode parsed = curl_url_set(url, CURLUPART_URL, value.c_str(), 0);
    char* scheme = nullptr;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    const bool valid = parsed == CURLUE_OK &&
                       curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                       curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK && host && *host &&
                       (std::string(scheme) == "http" || std::string(scheme) == "https") &&
                       curl_url_get(url, CURLUPART_USER, &user, 0) != CURLUE_OK &&
                       curl_url_get(url, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK;
    if (scheme) curl_free(scheme);
    if (host) curl_free(host);
    if (user) curl_free(user);
    if (password) curl_free(password);
    curl_url_cleanup(url);
    return valid;
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::size_t receiveBody(char* bytes, std::size_t size, std::size_t count, void* userdata) {
    auto& response = *static_cast<Response*>(userdata);
    const std::size_t length = size * count;
    const std::size_t kept = std::min(length, kMaximumResponseBytes - response.body.size());
    response.body.append(bytes, kept);
    if (kept != length) response.truncated = true;
    return length;
}

std::size_t receiveHeader(char* bytes, std::size_t size, std::size_t count, void* userdata) {
    auto& response = *static_cast<Response*>(userdata);
    const std::size_t length = size * count;
    if (length > 8192) return length;
    std::string line(bytes, length);
    if (line.compare(0, 5, "HTTP/") == 0) {
        response.headers = Json::object();
        return length;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) return length;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (name == "content-type" || name == "content-length" || name == "location" ||
        name == "etag" || name == "last-modified" || name == "cache-control") {
        response.headers[name] = trim(line.substr(colon + 1));
    }
    return length;
}

int checkProgress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& response = *static_cast<Response*>(userdata);
    return response.context && response.context->isCanceled() ? 1 : 0;
}

std::string encodeBase64(const std::string& source) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((source.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < source.size(); i += 3) {
        const auto a = static_cast<unsigned char>(source[i]);
        const auto b = i + 1 < source.size() ? static_cast<unsigned char>(source[i + 1]) : 0;
        const auto c = i + 2 < source.size() ? static_cast<unsigned char>(source[i + 2]) : 0;
        output.push_back(alphabet[a >> 2]);
        output.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        output.push_back(i + 1 < source.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        output.push_back(i + 2 < source.size() ? alphabet[c & 63] : '=');
    }
    return output;
}

bool validHeader(const std::string& name, const std::string& value) {
    if (name.empty() || name.size() > 128 || value.size() > 4096) return false;
    for (unsigned char ch : name)
        if (!std::isalnum(ch) && ch != '-') return false;
    for (unsigned char ch : value)
        if (ch == '\r' || ch == '\n' || ch == 0) return false;
    return true;
}

class HttpRequestTool final : public MaiTool {
public:
    explicit HttpRequestTool(std::string caBundlePath) : mCaBundlePath(std::move(caBundlePath)) {}

    std::string name() const override {
        return "curl_request";
    }
    std::string description() const override {
        return "Send a bounded HTTP(S) request using libcurl. Use GET/HEAD to inspect an API, "
               "or POST/PUT/PATCH/DELETE for explicitly requested API actions. Returns status, "
               "selected headers, body, and timing. Use curl_download for large responses and "
               "curl_upload for a local binary request body. "
               "Every call requires approval; never put passwords or private keys in arguments.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"url":{"type":"string"},"method":{"type":"string","enum":["GET","HEAD","POST","PUT","PATCH","DELETE"]},"headers":{"type":"object","additionalProperties":{"type":"string"}},"body":{"type":"string"},"timeout_seconds":{"type":"integer","minimum":1,"maximum":120}},"required":["url"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "expected JSON arguments");
        if (!args.value("url", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "url is required");
        const std::string url = args["url"].get<std::string>();
        if (url.size() > 8192 || !isHttpUrl(url))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "url must be HTTP(S) without embedded credentials");
        if ((args.contains("method") && !args["method"].is_string()) ||
            (args.contains("body") && !args["body"].is_string()) ||
            (args.contains("timeout_seconds") && !args["timeout_seconds"].is_number_integer()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "method, body, or timeout has the wrong type");
        const std::string method = args.value("method", std::string("GET"));
        if (method != "GET" && method != "HEAD" && method != "POST" && method != "PUT" &&
            method != "PATCH" && method != "DELETE")
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unsupported HTTP method");
        const std::string body = args.value("body", std::string{});
        if (body.size() > kMaximumRequestBytes ||
            (!body.empty() && (method == "GET" || method == "HEAD")))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "body is too large or invalid for GET/HEAD");
        const int timeout = args.value("timeout_seconds", 30);
        if (timeout < 1 || timeout > 120)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid timeout_seconds");
        Json headers = args.value("headers", Json::object());
        if (!headers.is_object() || headers.size() > 20)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "headers must be an object with at most 20 fields");

        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 curl_easy_cleanup);
        if (!curl)
            return MaiToolResult::failure(MaiErrorCode::Internal, "curl initialization failed");
        curl_slist* rawHeaders = nullptr;
        for (auto item = headers.begin(); item != headers.end(); ++item) {
            if (!item.value().is_string() ||
                !validHeader(item.key(), item.value().get<std::string>())) {
                curl_slist_free_all(rawHeaders);
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid HTTP header");
            }
            const std::string line = item.key() + ": " + item.value().get<std::string>();
            rawHeaders = curl_slist_append(rawHeaders, line.c_str());
        }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> ownedHeaders(
            rawHeaders, curl_slist_free_all);
        Response response;
        response.context = &context;
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, ownedHeaders.get());
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        if (method == "HEAD") curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
        if (method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE") {
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(body.size()));
        }
        if (!mCaBundlePath.empty())
            curl_easy_setopt(curl.get(), CURLOPT_CAINFO, mCaBundlePath.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receiveBody);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, receiveHeader);
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, checkProgress);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &response);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout));
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "MaiAgent/0.1");
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        maiAssertBlockingAllowed("curl_request");
        const CURLcode code = curl_easy_perform(curl.get());
        if (code != CURLE_OK) {
            return MaiToolResult::failure(
                context.isCanceled() ? MaiErrorCode::Canceled : MaiErrorCode::Network,
                curl_easy_strerror(code));
        }
        long status = 0;
        double totalSeconds = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl.get(), CURLINFO_TOTAL_TIME, &totalSeconds);
        Json output = {{"status", status},
                       {"headers", response.headers},
                       {"total_ms", static_cast<int>(totalSeconds * 1000)},
                       {"truncated", response.truncated}};
        try {
            Json utf8 = response.body;
            utf8.dump();
            output["body"] = std::move(utf8);
        } catch (const Json::type_error&) {
            output["body_base64"] = encodeBase64(response.body);
        }
        return MaiToolResult::success(output.dump(-1, ' ', false, Json::error_handler_t::replace),
                                      response.truncated);
    }

private:
    std::string mCaBundlePath;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiHttpRequestTool(std::string caBundlePath) {
    return std::make_unique<HttpRequestTool>(std::move(caBundlePath));
}
