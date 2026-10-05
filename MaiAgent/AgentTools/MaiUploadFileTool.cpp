#include "MaiUploadFileTool.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;
constexpr std::uint64_t kMegabyte = 1024 * 1024;
constexpr std::size_t kMaximumResponseBytes = 16 * 1024;

bool isHttpUrlWithoutCredentials(const std::string& value) {
    if (value.empty() || value.size() > 8192) return false;
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

bool validContentType(const std::string& type) {
    if (type.empty() || type.size() > 128) return false;
    return std::all_of(type.begin(), type.end(),
                       [](unsigned char ch) { return ch >= 33 && ch <= 126 && ch != ':'; });
}

class UploadSource {
public:
    explicit UploadSource(const MaiFilePath& path) {
#if defined(_WIN32)
        mHandle = CreateFileW(path.value().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
#else
        mDescriptor = ::open(path.value().c_str(), O_RDONLY | O_CLOEXEC);
#endif
    }

    ~UploadSource() {
#if defined(_WIN32)
        if (mHandle != INVALID_HANDLE_VALUE) CloseHandle(mHandle);
#else
        if (mDescriptor >= 0) ::close(mDescriptor);
#endif
    }

    bool isOpen() const {
#if defined(_WIN32)
        return mHandle != INVALID_HANDLE_VALUE;
#else
        return mDescriptor >= 0;
#endif
    }

    std::size_t read(char* output, std::size_t capacity) {
        if (capacity == 0) return 0;
#if defined(_WIN32)
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(capacity, 1024 * 1024));
        DWORD received = 0;
        if (!ReadFile(mHandle, output, requested, &received, nullptr)) {
            mReadFailed = true;
            return CURL_READFUNC_ABORT;
        }
        return received;
#else
        while (true) {
            const ssize_t received =
                ::read(mDescriptor, output, std::min<std::size_t>(capacity, 1024 * 1024));
            if (received >= 0) return static_cast<std::size_t>(received);
            if (errno == EINTR) continue;
            mReadFailed = true;
            return CURL_READFUNC_ABORT;
        }
#endif
    }

    bool readFailed() const {
        return mReadFailed;
    }

private:
#if defined(_WIN32)
    HANDLE mHandle = INVALID_HANDLE_VALUE;
#else
    int mDescriptor = -1;
#endif
    bool mReadFailed = false;
};

struct Transfer {
    UploadSource* source = nullptr;
    const MaiToolContext* context = nullptr;
    std::string response;
    bool responseTruncated = false;
};

std::size_t readUpload(char* bytes, std::size_t size, std::size_t count, void* userData) {
    auto& transfer = *static_cast<Transfer*>(userData);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
        return CURL_READFUNC_ABORT;
    return transfer.source->read(bytes, size * count);
}

std::size_t receiveResponse(char* bytes, std::size_t size, std::size_t count, void* userData) {
    auto& transfer = *static_cast<Transfer*>(userData);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const std::size_t length = size * count;
    const std::size_t kept = std::min(length, kMaximumResponseBytes - transfer.response.size());
    transfer.response.append(bytes, kept);
    if (kept != length) transfer.responseTruncated = true;
    return length;
}

int checkCanceled(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& transfer = *static_cast<Transfer*>(userData);
    return transfer.context->isCanceled() ? 1 : 0;
}

class UploadFileTool final : public MaiTool {
public:
    explicit UploadFileTool(std::string caBundlePath) : mCaBundlePath(std::move(caBundlePath)) {}

    std::string name() const override {
        return "curl_upload";
    }
    std::string description() const override {
        return "Stream an existing workspace file as the raw body of one HTTP(S) PUT or POST "
               "request, for example to a user-approved short-lived upload URL. This is not "
               "multipart form upload. The source remains unchanged; the remote host and file "
               "path need approval for every call. Never put permanent credentials in the URL.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"url":{"type":"string"},"path":{"type":"string"},"method":{"type":"string","enum":["PUT","POST"]},"content_type":{"type":"string"},"max_size_mb":{"type":"integer","minimum":1,"maximum":1024},"timeout_s":{"type":"integer","minimum":1,"maximum":1800}},"required":["url","path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("url", Json{}).is_string() ||
            !args.value("path", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "url and path are required strings");
        const std::string url = args["url"].get<std::string>();
        if (!isHttpUrlWithoutCredentials(url))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "url must be HTTP(S) without embedded credentials");
        if ((args.contains("method") && !args["method"].is_string()) ||
            (args.contains("content_type") && !args["content_type"].is_string()) ||
            (args.contains("max_size_mb") && !args["max_size_mb"].is_number_integer()) ||
            (args.contains("timeout_s") && !args["timeout_s"].is_number_integer()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid upload option");
        const std::string method = args.value("method", std::string("PUT"));
        const std::string contentType =
            args.value("content_type", std::string("application/octet-stream"));
        const int maximumMb = args.value("max_size_mb", 100);
        const int timeoutSeconds = args.value("timeout_s", 300);
        if ((method != "PUT" && method != "POST") || !validContentType(contentType) ||
            maximumMb < 1 || maximumMb > 1024 || timeoutSeconds < 1 || timeoutSeconds > 1800)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid upload option");

        const std::string path = context.resolvePath(args["path"].get<std::string>());
        if (path.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file is outside the accessible workspace");
        const MaiFilePath file = MaiFilePath::fromUtf8(path);
        std::uint64_t fileSize = 0;
        if (MaiFileSystem::isDirectory(file) || !MaiFileSystem::fileSize(file, fileSize))
            return MaiToolResult::failure(MaiErrorCode::NotFound, "upload file was not found");
        if (fileSize == 0 || fileSize > static_cast<std::uint64_t>(maximumMb) * kMegabyte)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "upload file is empty or exceeds max_size_mb");
        UploadSource source(file);
        if (!source.isOpen())
            return MaiToolResult::failure(MaiErrorCode::Internal, "cannot open upload file");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 curl_easy_cleanup);
        if (!curl)
            return MaiToolResult::failure(MaiErrorCode::Internal, "curl initialization failed");
        const std::string header = "Content-Type: " + contentType;
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
            curl_slist_append(nullptr, header.c_str()), curl_slist_free_all);
        if (!headers)
            return MaiToolResult::failure(MaiErrorCode::Internal, "cannot create upload header");
        Transfer transfer;
        transfer.source = &source;
        transfer.context = &context;
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, readUpload);
        curl_easy_setopt(curl.get(), CURLOPT_READDATA, &transfer);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receiveResponse);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, checkCanceled);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "MaiAgent/0.1");
        if (!mCaBundlePath.empty())
            curl_easy_setopt(curl.get(), CURLOPT_CAINFO, mCaBundlePath.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        if (method == "PUT") {
            curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE,
                             static_cast<curl_off_t>(fileSize));
        } else {
            curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(fileSize));
        }
        maiAssertBlockingAllowed("curl_upload");
        const CURLcode code = curl_easy_perform(curl.get());
        if (source.readFailed())
            return MaiToolResult::failure(MaiErrorCode::Internal, "upload file read failed");
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "upload was canceled");
        if (code != CURLE_OK)
            return MaiToolResult::failure(MaiErrorCode::Network, curl_easy_strerror(code));
        long status = 0;
        double totalSeconds = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl.get(), CURLINFO_TOTAL_TIME, &totalSeconds);
        if (status < 200 || status >= 300)
            return MaiToolResult::failure(MaiErrorCode::Network,
                                          "upload server returned HTTP " + std::to_string(status));
        Json output = {{"status", status},
                       {"bytes", fileSize},
                       {"total_ms", static_cast<int>(totalSeconds * 1000)},
                       {"response_truncated", transfer.responseTruncated}};
        output["response"] = transfer.response;
        return MaiToolResult::success(output.dump(-1, ' ', false, Json::error_handler_t::replace),
                                      transfer.responseTruncated);
    }

private:
    std::string mCaBundlePath;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiUploadFileTool(std::string caBundlePath) {
    return std::make_unique<UploadFileTool>(std::move(caBundlePath));
}
