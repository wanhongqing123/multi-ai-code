#include "MaiCurlDownload.h"

#include <curl/curl.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

struct MaiDownloadCancel {
    std::atomic_bool requested{false};
};

namespace {

struct Transfer {
    FILE* file = nullptr;
    uint64_t maximumBytes = 0;
    uint64_t bytes = 0;
    bool tooLarge = false;
    bool writeFailed = false;
    bool redirecting = false;
    const MaiDownloadCancel* cancel = nullptr;
};

bool validUrl(const char* url);

bool wasCanceled(const MaiDownloadCancel* cancel) {
    return cancel && cancel->requested.load(std::memory_order_relaxed);
}

size_t writeBytes(char* data, size_t size, size_t count, void* context) {
    auto* transfer = static_cast<Transfer*>(context);
    if (size != 0 && count > SIZE_MAX / size) {
        transfer->tooLarge = true;
        return 0;
    }
    const size_t length = size * count;
    if (wasCanceled(transfer->cancel)) return 0;
    // Redirect responses are not attachment data. Stop before reading a body.
    if (transfer->redirecting) return 0;
    if (length > transfer->maximumBytes - transfer->bytes) {
        transfer->tooLarge = true;
        return 0;
    }
    if (length != 0 && std::fwrite(data, 1, length, transfer->file) != length) {
        transfer->writeFailed = true;
        return 0;
    }
    transfer->bytes += length;
    return length;
}

int checkCanceled(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return wasCanceled(static_cast<Transfer*>(context)->cancel) ? 1 : 0;
}

struct ResponseHeaders {
    Transfer* transfer = nullptr;
    std::string location;
    long status = 0;
};

size_t receiveHeader(char* data, size_t size, size_t count, void* context) {
    if (size != 0 && count > SIZE_MAX / size) return 0;
    const size_t length = size * count;
    auto* headers = static_cast<ResponseHeaders*>(context);
    const std::string line(data, length);
    if (line.rfind("HTTP/", 0) == 0) {
        headers->location.clear();
        const size_t separator = line.find(' ');
        headers->status = separator == std::string::npos ? 0 :
            std::strtol(line.c_str() + separator + 1, nullptr, 10);
        headers->transfer->redirecting = headers->status == 301 || headers->status == 302 ||
            headers->status == 303 || headers->status == 307 || headers->status == 308;
    } else if (line.size() >= 9) {
        const char* prefix = "location:";
        bool location = true;
        for (size_t i = 0; i < 9; ++i) {
            const char c = line[i];
            if ((c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) != prefix[i]) {
                location = false;
                break;
            }
        }
        if (location) {
            const size_t first = line.find_first_not_of(" \t", 9);
            const size_t last = line.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last >= first)
                headers->location = line.substr(first, last - first + 1);
        }
    }
    return length;
}

std::string redirectedUrl(const std::string& current, const std::string& location) {
    if (location.empty() || location.size() > 16384) return {};
    // Keep absolute signed URLs byte-for-byte. Relative Location values are
    // resolved by libcurl's URL parser without decoding the query string.
    if (location.rfind("https://", 0) == 0)
        return validUrl(location.c_str()) ? location : std::string();
    CURLU* parsed = curl_url();
    if (!parsed) return {};
    std::string resolved;
    char* value = nullptr;
    if (curl_url_set(parsed, CURLUPART_URL, current.c_str(), 0) == CURLUE_OK &&
        curl_url_set(parsed, CURLUPART_URL, location.c_str(), 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_URL, &value, 0) == CURLUE_OK && value &&
        validUrl(value)) resolved = value;
    curl_free(value);
    curl_url_cleanup(parsed);
    return resolved;
}

bool validUrl(const char* url) {
    if (!url || std::strlen(url) > 16384) return false;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(url); *c; ++c) {
        if (*c < 0x20 || *c == 0x7f) return false;
    }
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    char* fragment = nullptr;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                       scheme && std::strcmp(scheme, "https") == 0 &&
                       curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                       host && *host &&
                       curl_url_get(parsed, CURLUPART_USER, &user, 0) != CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_FRAGMENT, &fragment, 0) != CURLUE_OK;
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_free(fragment);
    curl_url_cleanup(parsed);
    return valid;
}

#ifdef _WIN32
std::wstring toWide(const char* utf8) {
    if (!utf8) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring result(size_t(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, result.data(), length) != length)
        return {};
    result.resize(size_t(length - 1));
    return result;
}
#endif

class TemporaryFile {
public:
    explicit TemporaryFile(const char* finalPath) {
#ifdef _WIN32
        finalPath_ = toWide(finalPath);
        if (finalPath_.empty()) return;
        static std::atomic_uint64_t sequence{0};
        for (int attempt = 0; attempt < 16; ++attempt) {
            path_ = finalPath_ + L".maitmp-" + std::to_wstring(GetCurrentProcessId()) +
                    L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                    std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
            HANDLE handle = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                path_.clear();
                if (error != ERROR_FILE_EXISTS) break;
                continue;
            }
            const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY);
            if (fd < 0) {
                CloseHandle(handle);
                break;
            }
            file_ = _fdopen(fd, "wb");
            if (!file_) _close(fd);
            break;
        }
#else
        if (!finalPath || !*finalPath) return;
        finalPath_ = finalPath;
        path_ = finalPath_ + ".maitmp-XXXXXX";
        const int fd = mkstemp(path_.data());
        if (fd < 0) {
            path_.clear();
            return;
        }
        file_ = fdopen(fd, "wb");
        if (!file_) close(fd);
#endif
    }

    ~TemporaryFile() {
        if (file_) std::fclose(file_);
#ifdef _WIN32
        if (!path_.empty()) DeleteFileW(path_.c_str());
#else
        if (!path_.empty()) unlink(path_.c_str());
#endif
    }

    FILE* file() const { return file_; }

    bool commit() {
        if (!file_ || std::fflush(file_) != 0) return false;
#ifdef _WIN32
        const bool synced = _commit(_fileno(file_)) == 0;
#else
        const bool synced = fsync(fileno(file_)) == 0;
#endif
        const int closeResult = std::fclose(file_);
        if (!synced || closeResult != 0) {
            file_ = nullptr;
            return false;
        }
        file_ = nullptr;
#ifdef _WIN32
        if (!MoveFileExW(path_.c_str(), finalPath_.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
#else
        if (rename(path_.c_str(), finalPath_.c_str()) != 0) return false;
#endif
        path_.clear();
        return true;
    }

private:
    FILE* file_ = nullptr;
#ifdef _WIN32
    std::wstring finalPath_;
    std::wstring path_;
#else
    std::string finalPath_;
    std::string path_;
#endif
};

bool validOptions(const MaiDownloadOptions* options) {
    return options && validUrl(options->url_utf8) && options->target_path_utf8 &&
           *options->target_path_utf8 && options->max_bytes > 0 &&
           options->max_bytes <= uint64_t(std::numeric_limits<curl_off_t>::max()) &&
           (options->ca_pem || options->ca_pem_length == 0) &&
           !(options->ca_pem_length > 0 && options->ca_path_utf8 && *options->ca_path_utf8);
}

}  // namespace

extern "C" MaiDownloadCancel* mai_download_cancel_create(void) {
    return new (std::nothrow) MaiDownloadCancel();
}

extern "C" void mai_download_cancel_request(MaiDownloadCancel* cancel) {
    if (cancel) cancel->requested.store(true, std::memory_order_relaxed);
}

extern "C" void mai_download_cancel_destroy(MaiDownloadCancel* cancel) {
    delete cancel;
}

extern "C" void mai_download_file(const MaiDownloadOptions* options,
                                   const MaiDownloadCancel* cancel,
                                   MaiDownloadResult* result) {
    if (!result) return;
    *result = {MAI_DOWNLOAD_INVALID_ARGUMENT, 0, 0, 0};
    if (!validOptions(options)) return;
    if (wasCanceled(cancel)) {
        result->status = MAI_DOWNLOAD_CANCELED;
        return;
    }

    static std::once_flag initialization;
    static CURLcode initializationCode = CURLE_FAILED_INIT;
    std::call_once(initialization, [] { initializationCode = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initializationCode != CURLE_OK) {
        result->status = MAI_DOWNLOAD_NETWORK_ERROR;
        result->curl_code = initializationCode;
        return;
    }

    TemporaryFile output(options->target_path_utf8);
    if (!output.file()) {
        result->status = MAI_DOWNLOAD_WRITE_ERROR;
        return;
    }
    Transfer transfer{output.file(), options->max_bytes, 0, false, false, false, cancel};
    std::string currentUrl = options->url_utf8;
    for (int redirects = 0; redirects <= 3; ++redirects) {
        if (wasCanceled(cancel)) {
            result->status = MAI_DOWNLOAD_CANCELED;
            break;
        }
        MaiDownloadProxy selectedProxy{options->proxy_url_utf8,
                                       options->proxy_username_utf8,
                                       options->proxy_password_utf8};
        if (options->proxy_resolver &&
            options->proxy_resolver(currentUrl.c_str(), options->proxy_context, &selectedProxy) != 1) {
            result->status = MAI_DOWNLOAD_NETWORK_ERROR;
            result->curl_code = CURLE_COULDNT_RESOLVE_PROXY;
            break;
        }
        // The callback may return temporary strings. Own them through the hop.
        const bool hasProxy = selectedProxy.url_utf8 != nullptr;
        const bool hasUser = selectedProxy.username_utf8 != nullptr;
        const bool hasPassword = selectedProxy.password_utf8 != nullptr;
        const std::string proxy = hasProxy ? selectedProxy.url_utf8 : std::string();
        const std::string user = hasUser ? selectedProxy.username_utf8 : std::string();
        const std::string password = hasPassword ? selectedProxy.password_utf8 : std::string();
        CURL* curl = curl_easy_init();
        if (!curl) {
            result->status = MAI_DOWNLOAD_NETWORK_ERROR;
            result->curl_code = CURLE_FAILED_INIT;
            break;
        }
        transfer.redirecting = false;
        ResponseHeaders headers{&transfer, {}, 0};
        CURLcode setup = CURLE_OK;
        const auto set = [&](CURLoption key, auto value) {
            if (setup == CURLE_OK) setup = curl_easy_setopt(curl, key, value);
        };
        set(CURLOPT_URL, currentUrl.c_str());
        set(CURLOPT_PROTOCOLS_STR, "https");
        set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
        set(CURLOPT_FOLLOWLOCATION, 0L);
        set(CURLOPT_FAILONERROR, 1L);
        set(CURLOPT_HTTPGET, 1L);
        set(CURLOPT_CONNECTTIMEOUT_MS,
            long(options->connect_timeout_ms > 0 ? options->connect_timeout_ms : 10000));
        set(CURLOPT_TIMEOUT_MS, long(options->transfer_timeout_ms > 0
                                         ? options->transfer_timeout_ms
                                         : (options->max_bytes > 64ULL * 1024 * 1024 ? 600000 : 120000)));
        set(CURLOPT_LOW_SPEED_LIMIT, 1024L);
        set(CURLOPT_LOW_SPEED_TIME, 30L);
        set(CURLOPT_SSL_VERIFYPEER, 1L);
        set(CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef _WIN32
        if (options->ca_pem_length == 0 &&
            (!options->ca_path_utf8 || !*options->ca_path_utf8))
            set(CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
#endif
        set(CURLOPT_NETRC, long(CURL_NETRC_IGNORED));
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_MAXFILESIZE_LARGE, curl_off_t(options->max_bytes));
        set(CURLOPT_ACCEPT_ENCODING, "identity");
        set(CURLOPT_WRITEFUNCTION, writeBytes);
        set(CURLOPT_WRITEDATA, &transfer);
        set(CURLOPT_HEADERFUNCTION, receiveHeader);
        set(CURLOPT_HEADERDATA, &headers);
        set(CURLOPT_NOPROGRESS, 0L);
        set(CURLOPT_XFERINFOFUNCTION, checkCanceled);
        set(CURLOPT_XFERINFODATA, &transfer);
        if (hasProxy) {
            set(CURLOPT_PROXY, proxy.c_str());
            if (!proxy.empty()) set(CURLOPT_NOPROXY, "");
        }
        if (hasUser) set(CURLOPT_PROXYUSERNAME, user.c_str());
        if (hasPassword) set(CURLOPT_PROXYPASSWORD, password.c_str());
        curl_blob caBlob{const_cast<char*>(options->ca_pem), options->ca_pem_length, CURL_BLOB_COPY};
        if (options->ca_pem_length > 0) set(CURLOPT_CAINFO_BLOB, &caBlob);
        if (options->ca_path_utf8 && *options->ca_path_utf8)
            set(CURLOPT_CAINFO, options->ca_path_utf8);

        CURLcode code = setup;
        long httpStatus = 0;
        if (setup == CURLE_OK && !wasCanceled(cancel)) {
            code = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        }
        curl_easy_cleanup(curl);
        result->http_status = httpStatus;
        result->curl_code = int(code);
        result->bytes = transfer.bytes;
        if (wasCanceled(cancel)) {
            result->status = MAI_DOWNLOAD_CANCELED;
            break;
        }
        if (transfer.redirecting && !headers.location.empty()) {
            const std::string nextUrl = redirectedUrl(currentUrl, headers.location);
            if (nextUrl.empty() || redirects == 3) {
                result->status = MAI_DOWNLOAD_HTTP_ERROR;
                break;
            }
            currentUrl = nextUrl;
            continue;
        }
        if (transfer.writeFailed) {
            result->status = MAI_DOWNLOAD_WRITE_ERROR;
        } else if (transfer.tooLarge || code == CURLE_FILESIZE_EXCEEDED) {
            result->status = MAI_DOWNLOAD_TOO_LARGE;
            result->curl_code = CURLE_FILESIZE_EXCEEDED;
        } else if (code != CURLE_OK && httpStatus == 0) {
            result->status = MAI_DOWNLOAD_NETWORK_ERROR;
        } else if (httpStatus != 200) {
            result->status = MAI_DOWNLOAD_HTTP_ERROR;
        } else if (code != CURLE_OK) {
            result->status = MAI_DOWNLOAD_NETWORK_ERROR;
        } else if (transfer.bytes == 0) {
            result->status = MAI_DOWNLOAD_EMPTY_BODY;
        } else if (!output.commit()) {
            result->status = MAI_DOWNLOAD_WRITE_ERROR;
        } else {
            result->status = MAI_DOWNLOAD_OK;
        }
        break;
    }
}
