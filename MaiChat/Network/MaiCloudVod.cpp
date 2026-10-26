#include "MaiCloudVod.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

struct MaiCloudVodCancel {
    std::atomic_bool requested{false};
};

struct CloudDriveRenewal {
    MaiCloudDriveOptions options{};
    std::string baseUrl;
    std::string token;
    std::string caPem;
    std::string caPath;
    std::string proxy;
    std::string proxyUser;
    std::string proxyPassword;
    std::string hostSuffix;
    std::string videoId;
    const MaiCloudDriveCancel* initialCancel = nullptr;
    mutable std::mutex cancelMutex;
    MaiCloudDriveCancel* activeCancel = nullptr;
    bool initialOpen = true;
    struct MaiCloudVod* source = nullptr;
    uint64_t readEpoch = 0;
    uint64_t expectedSize = 0;

    bool fetch(std::string* url, uint64_t* size,
               MaiCloudVodResult* result);
    void interruptActive();
};

struct MaiCloudVod {
    struct Chunk {
        uint64_t index = 0;
        uint64_t used = 0;
        std::vector<unsigned char> bytes;
    };

    std::string url;
    CURL* curl = nullptr;
    std::string hostSuffix;
    std::string caPem;
    std::string caPath;
    std::string proxy;
    std::string proxyUser;
    std::string proxyPassword;
    bool explicitProxy = false;
    bool explicitProxyUser = false;
    bool explicitProxyPassword = false;
    MaiDownloadProxyResolver proxyResolver = nullptr;
    void* proxyContext = nullptr;
    MaiCloudVodRefreshUrl refreshUrl = nullptr;
    void* refreshContext = nullptr;
    const MaiCloudVodCancel* cancel = nullptr;
    std::atomic_bool stopped{false};
    std::atomic<uint64_t> readGeneration{0};
    uint64_t expectedSize = 0;
    uint64_t size = 0;
    uint64_t accessSerial = 0;
    int connectTimeoutMs = 10000;
    int transferTimeoutMs = 90000;
    std::string etag;
    std::vector<Chunk> chunks;
    std::unique_ptr<CloudDriveRenewal> renewal;

    ~MaiCloudVod() {
        if (curl) curl_easy_cleanup(curl);
    }
};

namespace {

constexpr uint64_t kMaximumBytes = 2'000'000'000;
constexpr uint64_t kChunkBytes = 1024 * 1024;
constexpr size_t kCacheChunks = 8;
constexpr size_t kMaximumUrlBytes = 16384;
constexpr size_t kMaximumHeaderBytes = 32768;

std::unique_ptr<CloudDriveRenewal> makeRenewal(
    const MaiCloudDriveOptions* options, const char* videoId,
    const MaiCloudDriveCancel* cancel) {
    if (!options || !videoId || !*videoId) return nullptr;
    auto owned = std::make_unique<CloudDriveRenewal>();
    owned->baseUrl = options->auth_base_url_utf8 ? options->auth_base_url_utf8 : "";
    owned->token = options->account_token_utf8 ? options->account_token_utf8 : "";
    if (options->ca_pem_length && options->ca_pem)
        owned->caPem.assign(options->ca_pem, options->ca_pem_length);
    owned->caPath = options->ca_path_utf8 ? options->ca_path_utf8 : "";
    owned->proxy = options->proxy_url_utf8 ? options->proxy_url_utf8 : "";
    owned->proxyUser = options->proxy_username_utf8 ? options->proxy_username_utf8 : "";
    owned->proxyPassword = options->proxy_password_utf8 ?
        options->proxy_password_utf8 : "";
    owned->hostSuffix = options->signed_upload_host_suffix_utf8 ?
        options->signed_upload_host_suffix_utf8 : "aliyuncs.com";
    owned->videoId = videoId;
    owned->initialCancel = cancel;
    owned->options = *options;
    owned->options.auth_base_url_utf8 = owned->baseUrl.c_str();
    owned->options.account_token_utf8 = owned->token.c_str();
    owned->options.ca_pem = owned->caPem.empty() ? nullptr : owned->caPem.data();
    owned->options.ca_pem_length = owned->caPem.size();
    owned->options.ca_path_utf8 = owned->caPath.empty() ? nullptr : owned->caPath.c_str();
    owned->options.proxy_url_utf8 = options->proxy_url_utf8 ? owned->proxy.c_str() : nullptr;
    owned->options.proxy_username_utf8 = options->proxy_username_utf8 ?
        owned->proxyUser.c_str() : nullptr;
    owned->options.proxy_password_utf8 = options->proxy_password_utf8 ?
        owned->proxyPassword.c_str() : nullptr;
    owned->options.signed_upload_host_suffix_utf8 = owned->hostSuffix.c_str();
    return owned;
}

int refreshCloudDriveUrl(void* context, char* output, size_t capacity) {
    auto* renewal = static_cast<CloudDriveRenewal*>(context);
    std::string fresh;
    uint64_t size = 0;
    MaiCloudVodResult result{};
    if (!renewal->fetch(&fresh, &size, &result) ||
        size != renewal->expectedSize || fresh.size() + 1 > capacity)
        return 0;
    std::memcpy(output, fresh.c_str(), fresh.size() + 1);
    return 1;
}

bool canceled(const MaiCloudVod* source) {
    return source->stopped.load(std::memory_order_relaxed) ||
        (source->cancel && source->cancel->requested.load(std::memory_order_relaxed));
}

MaiCloudVodStatus abortStatus(const MaiCloudVod* source, uint64_t epoch) {
    if (canceled(source)) return MAI_CLOUD_VOD_CANCELED;
    return source->readGeneration.load(std::memory_order_relaxed) != epoch
        ? MAI_CLOUD_VOD_INTERRUPTED : MAI_CLOUD_VOD_OK;
}

void reset(MaiCloudVodResult* result) {
    if (result) *result = {MAI_CLOUD_VOD_INVALID_ARGUMENT, 0, 0};
}

bool validHostSuffix(const std::string& suffix) {
    return !suffix.empty() && suffix.size() <= 253 &&
        std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) {
            return std::islower(ch) || std::isdigit(ch) || ch == '-' || ch == '.';
        });
}

bool validUrl(const char* url, const std::string& hostSuffix) {
    if (!url || !*url || std::strlen(url) > kMaximumUrlBytes) return false;
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    char* fragment = nullptr;
    char* query = nullptr;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        scheme && std::strcmp(scheme, "https") == 0 &&
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        host && *host &&
        curl_url_get(parsed, CURLUPART_USER, &user, 0) != CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_FRAGMENT, &fragment, 0) != CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_QUERY, &query, 0) == CURLUE_OK && query && *query;
    std::string lowerHost = host ? host : "";
    std::transform(lowerHost.begin(), lowerHost.end(), lowerHost.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    const bool hostMatches = validHostSuffix(hostSuffix) &&
        (lowerHost == hostSuffix ||
         (lowerHost.size() > hostSuffix.size() &&
          lowerHost.compare(lowerHost.size() - hostSuffix.size(),
                            hostSuffix.size(), hostSuffix) == 0 &&
          lowerHost[lowerHost.size() - hostSuffix.size() - 1] == '.'));
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_free(fragment);
    curl_free(query);
    curl_url_cleanup(parsed);
    return valid && hostMatches;
}

std::string redirectedUrl(const std::string& current, const std::string& location,
                          const std::string& hostSuffix) {
    if (location.empty() || location.size() > kMaximumUrlBytes) return {};
    CURLU* parsed = curl_url();
    if (!parsed) return {};
    std::string resolved;
    char* value = nullptr;
    if (curl_url_set(parsed, CURLUPART_URL, current.c_str(), 0) == CURLUE_OK &&
        curl_url_set(parsed, CURLUPART_URL, location.c_str(), 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_URL, &value, 0) == CURLUE_OK &&
        validUrl(value, hostSuffix)) resolved = value;
    curl_free(value);
    curl_url_cleanup(parsed);
    return resolved;
}

bool parseDecimal(const char*& cursor, uint64_t* value) {
    if (!std::isdigit(static_cast<unsigned char>(*cursor))) return false;
    uint64_t number = 0;
    do {
        const unsigned digit = unsigned(*cursor - '0');
        if (number > (UINT64_MAX - digit) / 10) return false;
        number = number * 10 + digit;
        ++cursor;
    } while (std::isdigit(static_cast<unsigned char>(*cursor)));
    *value = number;
    return true;
}

bool parseContentRange(const std::string& value, uint64_t* first,
                       uint64_t* last, uint64_t* total) {
    const char* cursor = value.c_str();
    if (std::strncmp(cursor, "bytes ", 6) != 0) return false;
    cursor += 6;
    if (!parseDecimal(cursor, first) || *cursor++ != '-' ||
        !parseDecimal(cursor, last) || *cursor++ != '/' ||
        !parseDecimal(cursor, total)) return false;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    return (*cursor == '\r' || *cursor == '\n' || *cursor == '\0') &&
        *total > 0 && *first <= *last && *last < *total;
}

std::string trimHeader(const std::string& line, size_t begin) {
    const size_t first = line.find_first_not_of(" \t", begin);
    if (first == std::string::npos) return {};
    const size_t last = line.find_last_not_of(" \t\r\n");
    return last < first ? std::string() : line.substr(first, last - first + 1);
}

struct Response {
    MaiCloudVod* source = nullptr;
    uint64_t epoch = 0;
    size_t headerBytes = 0;
    size_t maximumBody = 0;
    long status = 0;
    bool tooLarge = false;
    bool invalidHeader = false;
    std::string contentRange;
    std::string contentEncoding;
    std::string location;
    std::string etag;
    std::vector<unsigned char> body;
};

size_t receiveHeader(char* data, size_t size, size_t count, void* context) {
    auto* response = static_cast<Response*>(context);
    if (size && count > SIZE_MAX / size) return 0;
    const size_t length = size * count;
    if (length > kMaximumHeaderBytes - response->headerBytes) {
        response->invalidHeader = true;
        return 0;
    }
    response->headerBytes += length;
    const std::string line(data, length);
    if (line.rfind("HTTP/", 0) == 0) {
        response->contentRange.clear();
        response->contentEncoding.clear();
        response->location.clear();
        response->etag.clear();
        const size_t separator = line.find(' ');
        if (separator != std::string::npos)
            response->status = std::strtol(line.c_str() + separator + 1, nullptr, 10);
        return length;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return length;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return char(std::tolower(ch));
    });
    const std::string value = trimHeader(line, colon + 1);
    if (name == "content-range") response->contentRange = value;
    else if (name == "content-encoding") response->contentEncoding = value;
    else if (name == "location") response->location = value;
    else if (name == "etag") response->etag = value;
    return length;
}

size_t receiveBody(char* data, size_t size, size_t count, void* context) {
    auto* response = static_cast<Response*>(context);
    if (size && count > SIZE_MAX / size) {
        response->tooLarge = true;
        return 0;
    }
    const size_t length = size * count;
    // Refuse a 200 response before it can turn a range request into a full
    // download. Redirect and error bodies are never retained either.
    if (response->status != 206) return 0;
    if (length > response->maximumBody - response->body.size()) {
        response->tooLarge = true;
        return 0;
    }
    response->body.insert(response->body.end(), data, data + length);
    return length;
}

int checkCanceled(void* context, curl_off_t, curl_off_t,
                  curl_off_t, curl_off_t) {
    auto* response = static_cast<Response*>(context);
    return abortStatus(response->source, response->epoch) != MAI_CLOUD_VOD_OK ? 1 : 0;
}

bool selectProxy(MaiCloudVod* source, const std::string& url,
                 std::string* proxy, std::string* user, std::string* password,
                 bool* hasProxy, bool* hasUser, bool* hasPassword) {
    MaiDownloadProxy selected{
        source->explicitProxy ? source->proxy.c_str() : nullptr,
        source->explicitProxyUser ? source->proxyUser.c_str() : nullptr,
        source->explicitProxyPassword ? source->proxyPassword.c_str() : nullptr};
    if (source->proxyResolver &&
        source->proxyResolver(url.c_str(), source->proxyContext, &selected) != 1)
        return false;
    *hasProxy = selected.url_utf8 != nullptr;
    *hasUser = selected.username_utf8 != nullptr;
    *hasPassword = selected.password_utf8 != nullptr;
    if (*hasProxy) *proxy = selected.url_utf8;
    if (*hasUser) *user = selected.username_utf8;
    if (*hasPassword) *password = selected.password_utf8;
    return true;
}

bool renewUrl(MaiCloudVod* source, uint64_t epoch) {
    if (!source->refreshUrl || abortStatus(source, epoch) != MAI_CLOUD_VOD_OK)
        return false;
    if (source->renewal) source->renewal->readEpoch = epoch;
    else if (source->refreshUrl == refreshCloudDriveUrl && source->refreshContext)
        static_cast<CloudDriveRenewal*>(source->refreshContext)->readEpoch = epoch;
    char fresh[kMaximumUrlBytes + 1]{};
    if (source->refreshUrl(source->refreshContext, fresh, sizeof(fresh)) != 1 ||
        !std::memchr(fresh, 0, sizeof(fresh)) ||
        !validUrl(fresh, source->hostSuffix)) return false;
    source->url = fresh;
    return true;
}

bool fetchChunk(MaiCloudVod* source, uint64_t index, uint64_t epoch,
                MaiCloudVodResult* result,
                std::vector<unsigned char>* output, bool probeSize = false) {
    const uint64_t first = index * kChunkBytes;
    const uint64_t knownSize = source->size ? source->size : source->expectedSize;
    if ((knownSize && first >= knownSize) || (!knownSize && !probeSize)) {
        result->status = MAI_CLOUD_VOD_INVALID_ARGUMENT;
        return false;
    }
    // OSS may ignore a Range whose end is beyond EOF and return 200/full.
    // A size-unknown caller first probes exactly byte zero below, then uses
    // the authoritative Content-Range total for all subsequent chunks.
    const uint64_t last = probeSize ? 0 :
        std::min(first + kChunkBytes, knownSize) - 1;
    const std::string range = std::to_string(first) + "-" + std::to_string(last);
    std::string currentUrl = source->url;
    int redirects = 0;
    bool renewed = false;
    while (abortStatus(source, epoch) == MAI_CLOUD_VOD_OK) {
        std::string proxy;
        std::string user;
        std::string password;
        bool hasProxy = false;
        bool hasUser = false;
        bool hasPassword = false;
        if (!selectProxy(source, currentUrl, &proxy, &user, &password,
                         &hasProxy, &hasUser, &hasPassword)) {
            result->status = MAI_CLOUD_VOD_NETWORK_ERROR;
            result->curl_code = CURLE_COULDNT_RESOLVE_PROXY;
            return false;
        }
        CURL* curl = source->curl;
        if (!curl) {
            result->status = MAI_CLOUD_VOD_NETWORK_ERROR;
            result->curl_code = CURLE_FAILED_INIT;
            return false;
        }
        // curl_easy_reset preserves this handle's live connection and DNS
        // cache, while requiring every per-request option to be set again.
        // One playback source has one reader, so this handle is never used
        // concurrently. A complete 206 body can reuse the same TLS socket.
        curl_easy_reset(curl);
        Response response;
        response.source = source;
        response.epoch = epoch;
        response.maximumBody = size_t(kChunkBytes);
        response.body.reserve(size_t(kChunkBytes));
        curl_blob caBlob{source->caPem.empty() ? nullptr : source->caPem.data(),
                         source->caPem.size(), CURL_BLOB_COPY};
        CURLcode setup = CURLE_OK;
        const auto set = [&](CURLoption key, auto value) {
            if (setup == CURLE_OK) setup = curl_easy_setopt(curl, key, value);
        };
        set(CURLOPT_URL, currentUrl.c_str());
        set(CURLOPT_PROTOCOLS_STR, "https");
        set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
        set(CURLOPT_FOLLOWLOCATION, 0L);
        set(CURLOPT_RANGE, range.c_str());
        set(CURLOPT_HTTPGET, 1L);
        set(CURLOPT_NETRC, long(CURL_NETRC_IGNORED));
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_SSL_VERIFYPEER, 1L);
        set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_CONNECTTIMEOUT_MS, long(source->connectTimeoutMs));
        set(CURLOPT_TIMEOUT_MS, long(source->transferTimeoutMs));
        set(CURLOPT_LOW_SPEED_LIMIT, 1024L);
        set(CURLOPT_LOW_SPEED_TIME, 20L);
        set(CURLOPT_ACCEPT_ENCODING, "identity");
        set(CURLOPT_WRITEFUNCTION, receiveBody);
        set(CURLOPT_WRITEDATA, &response);
        set(CURLOPT_HEADERFUNCTION, receiveHeader);
        set(CURLOPT_HEADERDATA, &response);
        set(CURLOPT_NOPROGRESS, 0L);
        set(CURLOPT_XFERINFOFUNCTION, checkCanceled);
        set(CURLOPT_XFERINFODATA, &response);
        if (!source->caPem.empty()) set(CURLOPT_CAINFO_BLOB, &caBlob);
        else if (!source->caPath.empty()) set(CURLOPT_CAINFO, source->caPath.c_str());
#ifdef _WIN32
        else set(CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
#endif
        if (hasProxy) {
            set(CURLOPT_PROXY, proxy.c_str());
            set(CURLOPT_NOPROXY, proxy.empty() ? "*" : "");
        }
        if (hasUser) set(CURLOPT_PROXYUSERNAME, user.c_str());
        if (hasPassword) set(CURLOPT_PROXYPASSWORD, password.c_str());
        long status = 0;
        CURLcode code = setup;
        if (setup == CURLE_OK && abortStatus(source, epoch) == MAI_CLOUD_VOD_OK) {
            code = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        }
        result->http_status = status;
        result->curl_code = int(code);
        if (abortStatus(source, epoch) != MAI_CLOUD_VOD_OK) {
            result->status = abortStatus(source, epoch);
            return false;
        }
        if (status >= 300 && status < 400 && !response.location.empty()) {
            const std::string next = redirectedUrl(currentUrl, response.location,
                                                   source->hostSuffix);
            if (next.empty() || redirects++ >= 3) {
                result->status = MAI_CLOUD_VOD_HTTP_ERROR;
                return false;
            }
            currentUrl = next;
            continue;
        }
        if (status == 403 && !renewed && renewUrl(source, epoch)) {
            currentUrl = source->url;
            renewed = true;
            redirects = 0;
            continue;
        }
        if (abortStatus(source, epoch) != MAI_CLOUD_VOD_OK) {
            result->status = abortStatus(source, epoch);
            return false;
        }
        if (response.tooLarge) {
            result->status = MAI_CLOUD_VOD_TOO_LARGE;
            return false;
        }
        if (response.invalidHeader) {
            result->status = MAI_CLOUD_VOD_INVALID_RESPONSE;
            return false;
        }
        if (status != 206) {
            result->status = status ? MAI_CLOUD_VOD_HTTP_ERROR :
                MAI_CLOUD_VOD_NETWORK_ERROR;
            return false;
        }
        if (code != CURLE_OK) {
            result->status = MAI_CLOUD_VOD_NETWORK_ERROR;
            return false;
        }
        uint64_t returnedFirst = 0;
        uint64_t returnedLast = 0;
        uint64_t total = 0;
        if (!parseContentRange(response.contentRange, &returnedFirst,
                               &returnedLast, &total) ||
            total > kMaximumBytes ||
            (source->expectedSize && source->expectedSize != total) ||
            (source->size && source->size != total) ||
            returnedFirst != first ||
            returnedLast != std::min(last, total - 1) ||
            response.body.size() != returnedLast - returnedFirst + 1 ||
            (!response.contentEncoding.empty() &&
             response.contentEncoding != "identity") ||
            response.etag.empty() ||
            (!source->etag.empty() && response.etag != source->etag)) {
            result->status = total > kMaximumBytes ? MAI_CLOUD_VOD_TOO_LARGE :
                MAI_CLOUD_VOD_INVALID_RESPONSE;
            return false;
        }
        source->size = total;
        if (source->etag.empty()) source->etag = response.etag;
        source->url = currentUrl;
        *output = std::move(response.body);
        result->status = MAI_CLOUD_VOD_OK;
        return true;
    }
    result->status = abortStatus(source, epoch);
    return false;
}

MaiCloudVod::Chunk* findChunk(MaiCloudVod* source, uint64_t index) {
    for (MaiCloudVod::Chunk& chunk : source->chunks) {
        if (chunk.index == index) {
            chunk.used = ++source->accessSerial;
            return &chunk;
        }
    }
    return nullptr;
}

MaiCloudVod::Chunk* loadChunk(MaiCloudVod* source, uint64_t index,
                              uint64_t epoch,
                              MaiCloudVodResult* result) {
    if (MaiCloudVod::Chunk* existing = findChunk(source, index)) return existing;
    std::vector<unsigned char> bytes;
    if (!fetchChunk(source, index, epoch, result, &bytes)) return nullptr;
    if (source->chunks.size() == kCacheChunks) {
        auto victim = std::min_element(source->chunks.begin(), source->chunks.end(),
            [](const MaiCloudVod::Chunk& left, const MaiCloudVod::Chunk& right) {
                return left.used < right.used;
            });
        *victim = {index, ++source->accessSerial, std::move(bytes)};
        return &*victim;
    }
    source->chunks.push_back({index, ++source->accessSerial, std::move(bytes)});
    return &source->chunks.back();
}

}  // namespace

void CloudDriveRenewal::interruptActive() {
    std::lock_guard<std::mutex> lock(cancelMutex);
    if (activeCancel) mai_cloud_drive_cancel_request(activeCancel);
}

bool CloudDriveRenewal::fetch(std::string* url, uint64_t* size,
                              MaiCloudVodResult* result) {
    if (!result || !url || !size) return false;
    const std::string payload =
        nlohmann::json{{"video_id", videoId}}.dump();
    MaiCloudDriveCancel* requestCancel = nullptr;
    const MaiCloudDriveCancel* cancel = initialCancel;
    if (!initialOpen) {
        requestCancel = mai_cloud_drive_cancel_create();
        if (!requestCancel) {
            result->status = MAI_CLOUD_VOD_NETWORK_ERROR;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(cancelMutex);
            activeCancel = requestCancel;
            if (source && source->readGeneration.load(std::memory_order_relaxed) != readEpoch)
                mai_cloud_drive_cancel_request(requestCancel);
        }
        cancel = requestCancel;
    }
    MaiCloudDriveResult response{};
    mai_cloud_drive_request(&options, MAI_CLOUD_DRIVE_PLAY_LINK,
                            payload.c_str(), cancel, &response);
    if (requestCancel) {
        {
            std::lock_guard<std::mutex> lock(cancelMutex);
            activeCancel = nullptr;
        }
        mai_cloud_drive_cancel_destroy(requestCancel);
    }
    result->http_status = response.http_status;
    result->curl_code = response.curl_code;
    if (response.status != MAI_CLOUD_DRIVE_OK) {
        result->status = response.status == MAI_CLOUD_DRIVE_CANCELED ?
            MAI_CLOUD_VOD_CANCELED :
            response.status == MAI_CLOUD_DRIVE_HTTP_ERROR ?
                MAI_CLOUD_VOD_HTTP_ERROR :
            response.status == MAI_CLOUD_DRIVE_INVALID_RESPONSE ?
                MAI_CLOUD_VOD_INVALID_RESPONSE : MAI_CLOUD_VOD_NETWORK_ERROR;
        mai_cloud_drive_result_free(&response);
        return false;
    }
    const nlohmann::json body = response.response_json_utf8
        ? nlohmann::json::parse(response.response_json_utf8,
                                response.response_json_utf8 + response.response_bytes,
                                nullptr, false)
        : nlohmann::json();
    mai_cloud_drive_result_free(&response);
    if (!body.is_object() || !body.contains("read_url") ||
        !body["read_url"].is_string() || !body.contains("size_bytes") ||
        !body["size_bytes"].is_number_unsigned() ||
        !body.contains("media_type") || body["media_type"] != "video" ||
        !body.contains("content_type") ||
        (body["content_type"] != "video/mp4" &&
         body["content_type"] != "video/quicktime")) {
        result->status = MAI_CLOUD_VOD_INVALID_RESPONSE;
        return false;
    }
    *size = body["size_bytes"].get<uint64_t>();
    *url = body["read_url"].get<std::string>();
    if (*size == 0 || *size > kMaximumBytes ||
        (expectedSize && expectedSize != *size) ||
        !validUrl(url->c_str(), hostSuffix)) {
        result->status = *size > kMaximumBytes ? MAI_CLOUD_VOD_TOO_LARGE :
            MAI_CLOUD_VOD_INVALID_RESPONSE;
        return false;
    }
    result->status = MAI_CLOUD_VOD_OK;
    return true;
}

extern "C" MaiCloudVodCancel* mai_cloud_vod_cancel_create(void) {
    return new (std::nothrow) MaiCloudVodCancel;
}

extern "C" void mai_cloud_vod_cancel_request(MaiCloudVodCancel* cancel) {
    if (cancel) cancel->requested.store(true, std::memory_order_relaxed);
}

extern "C" void mai_cloud_vod_cancel_destroy(MaiCloudVodCancel* cancel) {
    delete cancel;
}

extern "C" MaiCloudVod* mai_cloud_vod_open(const MaiCloudVodOptions* options,
                                            MaiCloudVodResult* result) {
    reset(result);
    const std::string suffix = options && options->signed_host_suffix_utf8
        ? options->signed_host_suffix_utf8 : "aliyuncs.com";
    if (!result || !options || !validUrl(options->signed_url_utf8, suffix) ||
        options->expected_size_bytes > kMaximumBytes ||
        (options->ca_pem_length && !options->ca_pem) ||
        (options->ca_pem_length && options->ca_path_utf8 && *options->ca_path_utf8))
        return nullptr;
    static std::once_flag once;
    static CURLcode initialized = CURLE_FAILED_INIT;
    std::call_once(once, [] { initialized = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initialized != CURLE_OK) {
        result->status = MAI_CLOUD_VOD_NETWORK_ERROR;
        result->curl_code = int(initialized);
        return nullptr;
    }
    auto* source = new (std::nothrow) MaiCloudVod;
    if (!source) {
        result->status = MAI_CLOUD_VOD_NETWORK_ERROR;
        result->curl_code = CURLE_OUT_OF_MEMORY;
        return nullptr;
    }
    source->curl = curl_easy_init();
    if (!source->curl) {
        result->status = MAI_CLOUD_VOD_NETWORK_ERROR;
        result->curl_code = CURLE_FAILED_INIT;
        delete source;
        return nullptr;
    }
    source->url = options->signed_url_utf8;
    source->hostSuffix = suffix;
    if (options->ca_pem_length)
        source->caPem.assign(options->ca_pem, options->ca_pem_length);
    if (options->ca_path_utf8) source->caPath = options->ca_path_utf8;
    source->explicitProxy = options->proxy_url_utf8 != nullptr;
    if (source->explicitProxy) source->proxy = options->proxy_url_utf8;
    source->explicitProxyUser = options->proxy_username_utf8 != nullptr;
    if (source->explicitProxyUser) source->proxyUser = options->proxy_username_utf8;
    source->explicitProxyPassword = options->proxy_password_utf8 != nullptr;
    if (source->explicitProxyPassword) source->proxyPassword = options->proxy_password_utf8;
    source->proxyResolver = options->proxy_resolver;
    source->proxyContext = options->proxy_context;
    source->refreshUrl = options->refresh_url;
    source->refreshContext = options->refresh_context;
    source->cancel = options->cancel;
    source->expectedSize = options->expected_size_bytes;
    if (options->connect_timeout_ms > 0)
        source->connectTimeoutMs = options->connect_timeout_ms;
    if (options->transfer_timeout_ms > 0)
        source->transferTimeoutMs = options->transfer_timeout_ms;
    if (source->refreshUrl == refreshCloudDriveUrl && source->refreshContext)
        static_cast<CloudDriveRenewal*>(source->refreshContext)->source = source;
    if (!source->expectedSize) {
        std::vector<unsigned char> probe;
        if (!fetchChunk(source, 0, 0, result, &probe, true)) {
            delete source;
            return nullptr;
        }
    }
    if (!loadChunk(source, 0, 0, result)) {
        delete source;
        return nullptr;
    }
    return source;
}

extern "C" MaiCloudVod* mai_cloud_vod_open_cloud_drive(
    const MaiCloudDriveOptions* accountOptions, const char* videoId,
    const MaiCloudDriveCancel* accountCancel,
    const MaiCloudVodCancel* vodCancel, MaiCloudVodResult* result) {
    reset(result);
    if (!result || !accountOptions || !videoId || !*videoId) return nullptr;
    std::unique_ptr<CloudDriveRenewal> renewal =
        makeRenewal(accountOptions, videoId, accountCancel);
    if (!renewal) return nullptr;
    std::string url;
    uint64_t size = 0;
    if (!renewal->fetch(&url, &size, result)) return nullptr;
    renewal->expectedSize = size;
    MaiCloudVodOptions options{};
    options.signed_url_utf8 = url.c_str();
    options.signed_host_suffix_utf8 = renewal->hostSuffix.c_str();
    options.expected_size_bytes = size;
    options.ca_pem = renewal->options.ca_pem;
    options.ca_pem_length = renewal->options.ca_pem_length;
    options.ca_path_utf8 = renewal->options.ca_path_utf8;
    options.proxy_url_utf8 = renewal->options.proxy_url_utf8;
    options.proxy_username_utf8 = renewal->options.proxy_username_utf8;
    options.proxy_password_utf8 = renewal->options.proxy_password_utf8;
    options.proxy_resolver = renewal->options.proxy_resolver;
    options.proxy_context = renewal->options.proxy_context;
    options.refresh_url = refreshCloudDriveUrl;
    options.refresh_context = renewal.get();
    options.cancel = vodCancel;
    options.connect_timeout_ms = accountOptions->connect_timeout_ms;
    options.transfer_timeout_ms = accountOptions->transfer_timeout_ms;
    MaiCloudVod* source = mai_cloud_vod_open(&options, result);
    if (source) {
        renewal->initialOpen = false;
        renewal->initialCancel = nullptr;
        source->renewal = std::move(renewal);
    }
    return source;
}

extern "C" int mai_cloud_vod_read_at(MaiCloudVod* source, uint64_t offset,
                                      unsigned char* output, size_t capacity,
                                      size_t* bytesRead, MaiCloudVodResult* result) {
    reset(result);
    if (bytesRead) *bytesRead = 0;
    if (!source || !result || !bytesRead || (capacity && !output)) return 0;
    const uint64_t epoch = source->readGeneration.load(std::memory_order_relaxed);
    if (abortStatus(source, epoch) != MAI_CLOUD_VOD_OK) {
        result->status = abortStatus(source, epoch);
        return 0;
    }
    if (offset >= source->size || capacity == 0) {
        result->status = MAI_CLOUD_VOD_OK;
        return 1;
    }
    const uint64_t count = std::min(uint64_t(capacity), source->size - offset);
    size_t written = 0;
    while (written < count) {
        if (abortStatus(source, epoch) != MAI_CLOUD_VOD_OK) {
            result->status = abortStatus(source, epoch);
            return 0;
        }
        const uint64_t position = offset + written;
        MaiCloudVod::Chunk* chunk = loadChunk(source, position / kChunkBytes,
                                              epoch, result);
        if (!chunk) return 0;
        const size_t within = size_t(position % kChunkBytes);
        if (within >= chunk->bytes.size()) {
            result->status = MAI_CLOUD_VOD_INVALID_RESPONSE;
            return 0;
        }
        const size_t available = std::min(size_t(count - written),
                                          chunk->bytes.size() - within);
        std::memcpy(output + written, chunk->bytes.data() + within, available);
        written += available;
    }
    if (abortStatus(source, epoch) != MAI_CLOUD_VOD_OK) {
        result->status = abortStatus(source, epoch);
        return 0;
    }
    *bytesRead = written;
    result->status = MAI_CLOUD_VOD_OK;
    return 1;
}

extern "C" uint64_t mai_cloud_vod_size(const MaiCloudVod* source) {
    return source ? source->size : 0;
}

extern "C" uint64_t mai_cloud_vod_cached_bytes(const MaiCloudVod* source) {
    if (!source) return 0;
    uint64_t bytes = 0;
    for (const MaiCloudVod::Chunk& chunk : source->chunks) bytes += chunk.bytes.size();
    return bytes;
}

extern "C" void mai_cloud_vod_cancel(MaiCloudVod* source) {
    if (!source) return;
    source->stopped.store(true, std::memory_order_relaxed);
    if (source->renewal) source->renewal->interruptActive();
}

extern "C" void mai_cloud_vod_interrupt_read(MaiCloudVod* source) {
    if (!source) return;
    source->readGeneration.fetch_add(1, std::memory_order_relaxed);
    if (source->renewal) source->renewal->interruptActive();
}

extern "C" void mai_cloud_vod_close(MaiCloudVod* source) {
    delete source;
}
