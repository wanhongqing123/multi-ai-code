#include "MaiDownloadFileTool.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kMegabyte = 1024 * 1024;

MaiToolResult failure(MaiErrorCode errorCode, const char* code, const std::string& message,
                      long httpStatus = 0) {
    Json output = {{"code", code}, {"message", message}};
    if (httpStatus != 0) output["http_status"] = httpStatus;
    return MaiToolResult::failure(errorCode, output.dump());
}

std::string hostname(const std::string& url) {
    CURLU* parsed = curl_url();
    if (parsed == nullptr) return {};
    std::string result;
    if (curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        char* value = nullptr;
        if (curl_url_get(parsed, CURLUPART_HOST, &value, 0) == CURLUE_OK && value != nullptr) {
            result = value;
            curl_free(value);
        }
    }
    curl_url_cleanup(parsed);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

bool isHttpUrl(const std::string& url) {
    return (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0) &&
           !hostname(url).empty();
}

std::string safeFilename(std::string name) {
    if (name.size() > 255 || name == "." || name == "..") return {};
    if (name.empty()) return {};
    for (unsigned char ch : name) {
        if (ch < 32 || ch == 127 || ch == '/' || ch == '\\') return {};
    }
    return name;
}

std::string percentDecode(const std::string& source) {
    std::string output;
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (source[index] == '%' && index + 2 < source.size()) {
            const auto digit = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            const int high = digit(source[index + 1]);
            const int low = digit(source[index + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        output.push_back(source[index]);
    }
    return output;
}

std::string suggestedFilename(const std::string& disposition) {
    const std::string lower = [&] {
        std::string value = disposition;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }();
    for (const char* key : {"filename*=", "filename="}) {
        const std::size_t start = lower.find(key);
        if (start == std::string::npos) continue;
        std::string value = disposition.substr(start + std::strlen(key));
        value = value.substr(0, value.find(';'));
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                                  value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (std::strcmp(key, "filename*=") == 0) {
            const std::size_t encoding = value.find("''");
            if (encoding == std::string::npos || value.substr(0, encoding) != "UTF-8") continue;
            value = percentDecode(value.substr(encoding + 2));
        }
        value = safeFilename(value);
        if (!value.empty()) return value;
    }
    return {};
}

std::string filenameFromUrl(const std::string& url) {
    const std::size_t end = url.find_first_of("?#");
    const std::string path = url.substr(0, end);
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return safeFilename(percentDecode(path.substr(slash + 1)));
}

class MaiDownloadSink {
public:
    explicit MaiDownloadSink(MaiFilePath path) : mPath(std::move(path)) {}
    ~MaiDownloadSink() {
        discard();
    }

    bool open() {
#if defined(_WIN32)
        mHandle = CreateFileW(mPath.value().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mHandle == INVALID_HANDLE_VALUE)
            mDiskFull =
                GetLastError() == ERROR_DISK_FULL || GetLastError() == ERROR_HANDLE_DISK_FULL;
        return mHandle != INVALID_HANDLE_VALUE;
#else
        mDescriptor = ::open(mPath.value().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (mDescriptor < 0) mDiskFull = errno == ENOSPC;
        return mDescriptor >= 0;
#endif
    }

    bool write(const char* data, std::size_t size) {
        while (size > 0) {
#if defined(_WIN32)
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1024 * 1024));
            DWORD written = 0;
            if (!WriteFile(mHandle, data, chunk, &written, nullptr) || written == 0) {
                mDiskFull =
                    GetLastError() == ERROR_DISK_FULL || GetLastError() == ERROR_HANDLE_DISK_FULL;
                return false;
            }
#else
            const ssize_t written = ::write(mDescriptor, data, size);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                mDiskFull = errno == ENOSPC;
                return false;
            }
#endif
            data += written;
            size -= static_cast<std::size_t>(written);
        }
        return true;
    }

    bool close() {
#if defined(_WIN32)
        if (mHandle == INVALID_HANDLE_VALUE) return false;
        const bool flushed = FlushFileBuffers(mHandle) != 0;
        if (!flushed)
            mDiskFull =
                GetLastError() == ERROR_DISK_FULL || GetLastError() == ERROR_HANDLE_DISK_FULL;
        const bool closed = CloseHandle(mHandle) != 0;
        mHandle = INVALID_HANDLE_VALUE;
        return flushed && closed;
#else
        if (mDescriptor < 0) return false;
        const bool flushed = ::fsync(mDescriptor) == 0;
        if (!flushed) mDiskFull = errno == ENOSPC;
        const bool closed = ::close(mDescriptor) == 0;
        mDescriptor = -1;
        return flushed && closed;
#endif
    }

    bool publish(const MaiFilePath& destination) {
#if defined(_WIN32)
        const bool moved = MoveFileExW(mPath.value().c_str(), destination.value().c_str(),
                                       MOVEFILE_WRITE_THROUGH) != 0;
        if (!moved)
            mDiskFull =
                GetLastError() == ERROR_DISK_FULL || GetLastError() == ERROR_HANDLE_DISK_FULL;
#else
        const bool moved = ::link(mPath.value().c_str(), destination.value().c_str()) == 0 &&
                           ::unlink(mPath.value().c_str()) == 0;
        if (!moved) mDiskFull = errno == ENOSPC;
#endif
        if (moved) mPublished = true;
        return moved;
    }

    bool isDiskFull() const {
        return mDiskFull;
    }

private:
    void discard() {
#if defined(_WIN32)
        if (mHandle != INVALID_HANDLE_VALUE) CloseHandle(mHandle);
#else
        if (mDescriptor >= 0) ::close(mDescriptor);
#endif
        if (!mPublished) (void)MaiFileSystem::removeFile(mPath);
    }

    MaiFilePath mPath;
    bool mPublished = false;
    bool mDiskFull = false;
#if defined(_WIN32)
    HANDLE mHandle = INVALID_HANDLE_VALUE;
#else
    int mDescriptor = -1;
#endif
};

struct MaiDownloadTransfer {
    MaiDownloadSink* sink = nullptr;
    const MaiToolContext* context = nullptr;
    std::uint64_t maximumBytes = 0;
    std::uint64_t receivedBytes = 0;
    bool tooLarge = false;
    bool writeFailed = false;
    std::string contentDisposition;
};

std::size_t receiveBytes(char* data, std::size_t size, std::size_t count, void* userData) {
    auto& transfer = *static_cast<MaiDownloadTransfer*>(userData);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const std::size_t bytes = size * count;
    if (bytes > transfer.maximumBytes - transfer.receivedBytes) {
        transfer.tooLarge = true;
        return 0;
    }
    if (!transfer.sink->write(data, bytes)) {
        transfer.writeFailed = true;
        return 0;
    }
    transfer.receivedBytes += bytes;
    return bytes;
}

std::size_t receiveHeader(char* data, std::size_t size, std::size_t count, void* userData) {
    auto& transfer = *static_cast<MaiDownloadTransfer*>(userData);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const std::size_t bytes = size * count;
    const std::string line(data, bytes);
    if (line.compare(0, 5, "HTTP/") == 0) transfer.contentDisposition.clear();
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.compare(0, 20, "content-disposition:") == 0)
        transfer.contentDisposition = line.substr(20);
    return bytes;
}

int checkCanceled(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto& transfer = *static_cast<MaiDownloadTransfer*>(userData);
    return transfer.context->isCanceled() ? 1 : 0;
}

class MaiDownloadFileTool final : public MaiTool {
public:
    std::string name() const override {
        return "download_file";
    }
    std::string description() const override {
        return "Download a binary file from a direct HTTP or HTTPS URL into the Agent workspace. "
               "The file is streamed, size-limited, and never executed. If output_path is omitted, "
               "use the server filename hint or URL filename.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"url":{"type":"string"},"output_path":{"type":"string"},"max_size_mb":{"type":"number","minimum":1,"maximum":1024},"timeout_s":{"type":"number","minimum":1,"maximum":600}},"required":["url"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::string approvalKey(const std::string& raw) const override {
        const Json arguments = Json::parse(raw, nullptr, false);
        const std::string host = arguments.is_object() && arguments.value("url", Json{}).is_string()
                                     ? hostname(arguments["url"].get<std::string>())
                                     : std::string{};
        return "download_file:" + (host.empty() ? "<unknown>" : host);
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json arguments = Json::parse(raw, nullptr, false);
        if (!arguments.is_object() || !arguments.contains("url") || !arguments["url"].is_string())
            return failure(MaiErrorCode::InvalidInput, "invalid_url", "url must be an HTTP(S) URL");
        const std::string url = arguments["url"].get<std::string>();
        if (!isHttpUrl(url))
            return failure(MaiErrorCode::InvalidInput, "invalid_url", "url must be an HTTP(S) URL");
        if (context.root.empty())
            return failure(MaiErrorCode::NotConfigured, "invalid_path",
                           "Agent workspace is unavailable");
        const Json maxValue = arguments.value("max_size_mb", Json(100));
        const Json timeoutValue = arguments.value("timeout_s", Json(60));
        if (!maxValue.is_number() || !timeoutValue.is_number())
            return failure(MaiErrorCode::InvalidInput, "invalid_input",
                           "size and timeout must be numbers");
        const double maximumMb = maxValue.get<double>();
        const double timeoutSeconds = timeoutValue.get<double>();
        if (!std::isfinite(maximumMb) || maximumMb < 1 || maximumMb > 1024 ||
            !std::isfinite(timeoutSeconds) || timeoutSeconds < 1 || timeoutSeconds > 600)
            return failure(MaiErrorCode::InvalidInput, "invalid_input",
                           "size or timeout is out of range");
        if (arguments.contains("output_path") && !arguments["output_path"].is_string())
            return failure(MaiErrorCode::InvalidInput, "invalid_path",
                           "output_path must be a relative string");
        const std::string requested = arguments.value("output_path", Json{}).is_string()
                                          ? arguments["output_path"].get<std::string>()
                                          : std::string{};
        if (!requested.empty() && MaiFilePath::fromUtf8(requested).isAbsolute())
            return failure(MaiErrorCode::InvalidInput, "invalid_path",
                           "output_path must be relative");

        const std::string temporaryName =
            ".mai-download-" + MaiIdGenerator::generate("file_") + ".part";
        const std::string temporaryPath = maiResolvePathWithinRoot(context.root, temporaryName);
        if (temporaryPath.empty())
            return failure(MaiErrorCode::InvalidInput, "invalid_path",
                           "Agent workspace is inaccessible");
        MaiDownloadSink sink(MaiFilePath::fromUtf8(temporaryPath));
        if (!sink.open())
            return failure(MaiErrorCode::Internal, sink.isDiskFull() ? "disk_full" : "io_error",
                           "Cannot create a temporary download file");

        MaiDownloadTransfer transfer;
        transfer.sink = &sink;
        transfer.context = &context;
        transfer.maximumBytes = static_cast<std::uint64_t>(maximumMb * kMegabyte);
        maiAssertBlockingAllowed("download_file");
        CURL* curl = curl_easy_init();
        if (curl == nullptr)
            return failure(MaiErrorCode::Internal, "internal",
                           "Could not initialize the HTTP client");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveBytes);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receiveHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, checkCanceled);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutSeconds * 1000));
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "MaiAgent/0.1");
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

        const CURLcode curlResult = curl_easy_perform(curl);
        long httpStatus = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        char* contentType = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType);
        char* finalUrl = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &finalUrl);
        const std::string mime =
            contentType != nullptr ? std::string(contentType) : "application/octet-stream";
        const std::string landed = finalUrl != nullptr ? std::string(finalUrl) : url;
        curl_easy_cleanup(curl);

        if (transfer.tooLarge)
            return failure(MaiErrorCode::InvalidInput, "too_large", "Download exceeds max_size_mb");
        if (transfer.writeFailed)
            return failure(MaiErrorCode::Internal, sink.isDiskFull() ? "disk_full" : "io_error",
                           "Could not write the downloaded bytes");
        if (context.isCanceled())
            return failure(MaiErrorCode::Canceled, "canceled", "Download was canceled");
        if (curlResult == CURLE_OPERATION_TIMEDOUT)
            return failure(MaiErrorCode::Network, "timeout", "Download timed out");
        if (curlResult != CURLE_OK)
            return failure(MaiErrorCode::Network, "network_error", curl_easy_strerror(curlResult));
        if (httpStatus < 200 || httpStatus >= 300)
            return failure(MaiErrorCode::Network, "http_error", "Server returned an HTTP error",
                           httpStatus);
        if (!sink.close())
            return failure(MaiErrorCode::Internal, sink.isDiskFull() ? "disk_full" : "io_error",
                           "Could not finalize download file");

        std::string output = requested;
        if (output.empty()) output = suggestedFilename(transfer.contentDisposition);
        if (output.empty()) output = filenameFromUrl(landed);
        if (output.empty()) output = "download-" + MaiIdGenerator::generate("file_") + ".bin";
        if (output.empty() || MaiFilePath::fromUtf8(output).isAbsolute())
            return failure(MaiErrorCode::InvalidInput, "invalid_path",
                           "output_path must be relative");
        const std::string destinationPath = maiResolvePathWithinRoot(context.root, output);
        if (destinationPath.empty() || destinationPath == context.root)
            return failure(MaiErrorCode::InvalidInput, "invalid_path",
                           "output_path leaves the Agent workspace");
        const MaiFilePath destination = MaiFilePath::fromUtf8(destinationPath);
        if (MaiFileSystem::exists(destination))
            return failure(MaiErrorCode::InvalidInput, "file_exists", "output_path already exists");
        const MaiError directoryError = MaiFileSystem::createDirectories(destination.dirName());
        if (directoryError)
            return failure(MaiErrorCode::Internal, "io_error", directoryError.message());
        if (!sink.publish(destination))
            return failure(MaiErrorCode::Internal,
                           MaiFileSystem::exists(destination) ? "file_exists"
                           : sink.isDiskFull()                ? "disk_full"
                                                              : "io_error",
                           "Could not publish the download file");
        return MaiToolResult::success(Json{
            {"path", destinationPath},
            {"bytes", transfer.receivedBytes},
            {"mime_type", mime}}.dump());
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiDownloadFileTool() {
    return std::make_unique<MaiDownloadFileTool>();
}
