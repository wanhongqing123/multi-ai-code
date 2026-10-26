#include "MaiVideoCircleClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

struct MaiVideoCircleCancel {
    std::atomic_bool requested{false};
};

namespace {

using json = nlohmann::json;

constexpr uint64_t kMaxVideoBytes = 200'000'000;
constexpr size_t kMaxRequestBytes = 32 * 1024;
constexpr size_t kMaxResponseBytes = 1024 * 1024;
constexpr size_t kMaxHeaderBytes = 16 * 1024;

struct Response {
    std::string body;
    size_t headerBytes = 0;
    bool tooLarge = false;
    const MaiVideoCircleCancel* cancel = nullptr;
};

bool canceled(const MaiVideoCircleCancel* cancel) {
    return cancel && cancel->requested.load(std::memory_order_relaxed);
}

bool printable(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x20 && c != 0x7f;
    });
}

bool tokenChars(const std::string& value) {
    return !value.empty() && value.size() <= 8192 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return c >= 0x21 && c <= 0x7e;
           });
}

bool validBase(const char* raw, std::string* normalized) {
    if (!raw || !*raw || std::strlen(raw) > 2048 || !printable(raw)) return false;
    CURLU* url = curl_url();
    if (!url) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* query = nullptr;
    char* fragment = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    const bool valid = curl_url_set(url, CURLUPART_URL, raw, 0) == CURLUE_OK &&
                       curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                       scheme && std::strcmp(scheme, "https") == 0 &&
                       curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                       host && *host &&
                       curl_url_get(url, CURLUPART_QUERY, &query, 0) != CURLUE_OK &&
                       curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) != CURLUE_OK &&
                       curl_url_get(url, CURLUPART_USER, &user, 0) != CURLUE_OK &&
                       curl_url_get(url, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK &&
                       std::strstr(raw, "/../") == nullptr &&
                       std::strstr(raw, "/./") == nullptr;
    curl_free(scheme);
    curl_free(host);
    curl_free(query);
    curl_free(fragment);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(url);
    if (!valid) return false;
    *normalized = raw;
    while (normalized->size() > 8 && normalized->back() == '/') normalized->pop_back();
    return true;
}

bool validSignedUrl(const std::string& raw, const std::string& objectKey,
                    const char* hostSuffix) {
    if (raw.size() > 16384 || raw.empty() || !printable(raw)) return false;
    CURLU* url = curl_url();
    if (!url) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* path = nullptr;
    char* query = nullptr;
    char* fragment = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    const bool parsed = curl_url_set(url, CURLUPART_URL, raw.c_str(), 0) == CURLUE_OK &&
                        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                        scheme && std::strcmp(scheme, "https") == 0 &&
                        curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                        host && *host &&
                        curl_url_get(url, CURLUPART_PATH, &path, 0) == CURLUE_OK &&
                        path && objectKey == std::string(path).substr(1) &&
                        curl_url_get(url, CURLUPART_QUERY, &query, 0) == CURLUE_OK &&
                        query && *query &&
                        curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) != CURLUE_OK &&
                        curl_url_get(url, CURLUPART_USER, &user, 0) != CURLUE_OK &&
                        curl_url_get(url, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK;
    const std::string suffix = hostSuffix ? hostSuffix : "aliyuncs.com";
    std::string name = host ? host : "";
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    bool hostMatches = !suffix.empty() && suffix.size() <= 253 &&
                       std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
                           return std::isalnum(c) || c == '-' || c == '.';
                       });
    if (hostMatches) {
        hostMatches = name == suffix ||
                      (name.size() > suffix.size() &&
                       name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                       name[name.size() - suffix.size() - 1] == '.');
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(path);
    curl_free(query);
    curl_free(fragment);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(url);
    return parsed && hostMatches;
}

bool validIdentifier(const std::string& value) {
    return value.size() == 36 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
           });
}

bool validAccountId(const std::string& value) {
    if (value.size() < 3 || value.size() > 32 || value[0] < 'a' || value[0] > 'z')
        return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool validTaskId(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-';
           });
}

bool stringField(const json& object, const char* key, std::string* value) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) return false;
    *value = object[key].get<std::string>();
    return true;
}

bool integerField(const json& object, const char* key, uint64_t minimum,
                  uint64_t maximum, uint64_t* value) {
    if (!object.is_object() || !object.contains(key) ||
        !object[key].is_number_integer() ||
        (!object[key].is_number_unsigned() && object[key].get<int64_t>() < 0)) return false;
    const uint64_t supplied = object[key].get<uint64_t>();
    if (supplied < minimum || supplied > maximum) return false;
    *value = supplied;
    return true;
}

bool validTitle(const std::string& value) {
    if (value.empty() || value.size() > 240 || value.front() == ' ' ||
        value.back() == ' ') return false;
    return printable(value) &&
           std::count_if(value.begin(), value.end(), [](unsigned char c) {
               return (c & 0xc0) != 0x80;
           }) <= 80;
}

bool validRelativeMediaPath(const std::string& value) {
    return !value.empty() && value.size() <= 4096 && printable(value) &&
           value.front() != '/' && value.front() != '\\' &&
           value.find("..") == std::string::npos && value.find(':') == std::string::npos;
}

size_t receiveBody(char* bytes, size_t size, size_t count, void* context) {
    auto* response = static_cast<Response*>(context);
    if (canceled(response->cancel)) return 0;
    if (size != 0 && count > SIZE_MAX / size) {
        response->tooLarge = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > kMaxResponseBytes - response->body.size()) {
        response->tooLarge = true;
        return 0;
    }
    response->body.append(bytes, length);
    return length;
}

size_t receiveHeader(char*, size_t size, size_t count, void* context) {
    auto* response = static_cast<Response*>(context);
    if (size != 0 && count > SIZE_MAX / size) {
        response->tooLarge = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > kMaxHeaderBytes - response->headerBytes) {
        response->tooLarge = true;
        return 0;
    }
    response->headerBytes += length;
    return length;
}

int stopCanceled(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return canceled(static_cast<Response*>(context)->cancel) ? 1 : 0;
}

bool assignResponse(MaiVideoCircleResult* result, const std::string& value) {
    char* allocated = static_cast<char*>(std::malloc(value.size() + 1));
    if (!allocated) return false;
    std::memcpy(allocated, value.data(), value.size());
    allocated[value.size()] = '\0';
    result->response_json_utf8 = allocated;
    result->response_bytes = value.size();
    return true;
}

bool initializeCurl(MaiVideoCircleResult* result) {
    static std::once_flag initialization;
    static CURLcode code = CURLE_FAILED_INIT;
    std::call_once(initialization, [] { code = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (code == CURLE_OK) return true;
    result->status = MAI_VIDEO_CIRCLE_NETWORK_ERROR;
    result->curl_code = code;
    return false;
}

#ifdef _WIN32
std::wstring toWide(const char* utf8) {
    if (!utf8) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring wide(size_t(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide.data(), length) != length)
        return {};
    wide.resize(size_t(length - 1));
    return wide;
}
#endif

FILE* openFile(const char* path) {
    if (!path || !*path) return nullptr;
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    return wide.empty() ? nullptr : _wfopen(wide.c_str(), L"rb");
#else
    return std::fopen(path, "rb");
#endif
}

bool fileSize(FILE* file, uint64_t* output) {
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) return false;
    const auto size = _ftelli64(file);
    if (size < 0 || _fseeki64(file, 0, SEEK_SET) != 0) return false;
#else
    if (fseeko(file, 0, SEEK_END) != 0) return false;
    const auto size = ftello(file);
    if (size < 0 || fseeko(file, 0, SEEK_SET) != 0) return false;
#endif
    *output = uint64_t(size);
    return true;
}

size_t readFile(char* bytes, size_t size, size_t count, void* context) {
    if (size != 0 && count > SIZE_MAX / size) return CURL_READFUNC_ABORT;
    return std::fread(bytes, 1, size * count, static_cast<FILE*>(context));
}

bool validCard(const json& card, std::string* sourceTool, std::string* taskId,
               std::string* mediaPath) {
    std::string tool;
    std::string state;
    if (!stringField(card, "tool", &tool) || tool != "agent_send_media" ||
        !stringField(card, "state", &state) || state != "completed" ||
        !card.contains("output")) return false;
    json output = card["output"];
    if (output.is_string())
        output = json::parse(output.get<std::string>(), nullptr, false);
    std::string delivery;
    std::string type;
    std::string mime;
    if (!output.is_object() ||
        !stringField(output, "delivery", &delivery) ||
        !stringField(output, "type", &type) ||
        !stringField(output, "mime_type", &mime) ||
        !stringField(output, "path", mediaPath) ||
        !stringField(output, "source_tool", sourceTool) ||
        !stringField(output, "source_task_id", taskId) ||
        delivery != "current_ai_session" || type != "video" ||
        mime != "video/mp4" || !validRelativeMediaPath(*mediaPath) ||
        !validTaskId(*taskId)) return false;
    return *sourceTool == "wan_video" || *sourceTool == "kling_bailian_video" ||
           *sourceTool == "minimax_video" || *sourceTool == "seedance_video";
}

bool validFileName(const std::string& value) {
    return !value.empty() && value.size() <= 255 && printable(value) &&
           value != ".mp4" && value.substr(value.size() >= 4 ? value.size() - 4 : 0) == ".mp4" &&
           value.find('/') == std::string::npos && value.find('\\') == std::string::npos;
}

bool validObjectKey(const std::string& key) {
    if (key.size() < 58 || key.size() > 160 || key.rfind("video-circle/", 0) != 0 ||
        key.substr(key.size() - 4) != ".mp4") return false;
    const size_t ownerEnd = key.find('/', 13);
    if (ownerEnd == std::string::npos || !validAccountId(key.substr(13, ownerEnd - 13)))
        return false;
    const size_t dateEnd = key.find('/', ownerEnd + 1);
    if (dateEnd == std::string::npos || dateEnd - ownerEnd != 11 ||
        key[ownerEnd + 5] != '-' || key[ownerEnd + 8] != '-') return false;
    for (size_t i = ownerEnd + 1; i < dateEnd; ++i) {
        if (i != ownerEnd + 5 && i != ownerEnd + 8 &&
            (key[i] < '0' || key[i] > '9')) return false;
    }
    return validIdentifier(key.substr(dateEnd + 1, key.size() - dateEnd - 5));
}

bool buildRequest(MaiVideoCircleAction action, const json& input, const std::string& authBase,
                  const std::string& mediaBase, std::string* method,
                  std::string* url, std::string* body) {
    if (!input.is_object()) return false;
    *method = "GET";
    body->clear();
    if (action == MAI_VIDEO_CIRCLE_SIGN_UPLOAD) {
        std::string filename;
        uint64_t bytes = 0;
        if (mediaBase.empty() || !stringField(input, "filename", &filename) ||
            !validFileName(filename) ||
            !integerField(input, "size_bytes", 1, kMaxVideoBytes, &bytes)) return false;
        *method = "POST";
        *url = mediaBase + "/sign-upload";
        *body = json{{"filename", filename}, {"size_bytes", bytes},
                     {"purpose", "video_circle"}}.dump();
        return true;
    }
    if (authBase.empty()) return false;
    const std::string root = authBase + "/v1/video-circle";
    if (action == MAI_VIDEO_CIRCLE_PUBLISH_DRAFT) {
        std::string title;
        std::string key;
        std::string sourceTool;
        std::string taskId;
        std::string mediaPath;
        uint64_t bytes = 0;
        uint64_t duration = 0;
        if (!input.contains("completed_card") ||
            !validCard(input["completed_card"], &sourceTool, &taskId, &mediaPath) ||
            !stringField(input, "title", &title) || !validTitle(title) ||
            !stringField(input, "object_key", &key) || !validObjectKey(key) ||
            !integerField(input, "size_bytes", 1, kMaxVideoBytes, &bytes) ||
            !integerField(input, "duration_ms", 1, 60'000, &duration)) return false;
        *method = "POST";
        *url = root + "/posts";
        // Never send the local path, original prompt, or raw tool output.
        *body = json{{"title", title}, {"object_key", key},
                     {"source_tool", sourceTool}, {"source_task_id", taskId},
                     {"size_bytes", bytes}, {"duration_ms", duration},
                     {"ai_generated", true}}.dump();
        return true;
    }
    if (action == MAI_VIDEO_CIRCLE_FEED || action == MAI_VIDEO_CIRCLE_MINE) {
        *url = root + (action == MAI_VIDEO_CIRCLE_FEED ? "/feed" : "/mine");
        if (input.contains("before")) {
            std::string before;
            if (!stringField(input, "before", &before) || before.empty() ||
                before.size() > 160 ||
                !std::all_of(before.begin(), before.end(), [](unsigned char c) {
                    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
                })) return false;
            *url += "?before=" + before;
        }
        return true;
    }
    if (action == MAI_VIDEO_CIRCLE_BLOCKS_LIST) {
        *url = root + "/blocks";
        return true;
    }
    if (action == MAI_VIDEO_CIRCLE_BLOCK || action == MAI_VIDEO_CIRCLE_UNBLOCK) {
        std::string author;
        if (!stringField(input, "author_id", &author) || !validAccountId(author))
            return false;
        *method = "POST";
        *url = root + (action == MAI_VIDEO_CIRCLE_BLOCK ? "/blocks" : "/blocks/remove");
        *body = json{{"author_id", author}}.dump();
        return true;
    }
    std::string postId;
    if (!stringField(input, "post_id", &postId) || !validIdentifier(postId)) return false;
    const std::string post = root + "/posts/" + postId;
    if (action == MAI_VIDEO_CIRCLE_PLAY_LINK) {
        *url = post + "/play-link";
        return true;
    }
    *method = "POST";
    if (action == MAI_VIDEO_CIRCLE_WITHDRAW) {
        *url = post + "/withdraw";
        *body = "{}";
        return true;
    }
    if (action == MAI_VIDEO_CIRCLE_REPORT) {
        std::string reason;
        if (!stringField(input, "reason", &reason) ||
            (reason != "spam" && reason != "harassment" && reason != "sexual" &&
             reason != "violence" && reason != "copyright" && reason != "other"))
            return false;
        json report{{"reason", reason}};
        if (input.contains("details")) {
            std::string details;
            if (!stringField(input, "details", &details) || details.size() > 600 ||
                !printable(details)) return false;
            report["details"] = details;
        }
        *url = post + "/report";
        *body = report.dump();
        return true;
    }
    return false;
}

bool configureProxy(CURL* curl, const MaiVideoCircleOptions* options,
                    const std::string& url, MaiVideoCircleResult* result,
                    std::string* proxy, std::string* user, std::string* password) {
    MaiDownloadProxy selected{options->proxy_url_utf8, options->proxy_username_utf8,
                              options->proxy_password_utf8};
    if (options->proxy_resolver &&
        options->proxy_resolver(url.c_str(), options->proxy_context, &selected) != 1) {
        result->status = MAI_VIDEO_CIRCLE_NETWORK_ERROR;
        result->curl_code = CURLE_COULDNT_RESOLVE_PROXY;
        return false;
    }
    if (selected.url_utf8) {
        *proxy = selected.url_utf8;
        if (curl_easy_setopt(curl, CURLOPT_PROXY, proxy->c_str()) != CURLE_OK) return false;
        if (proxy->empty()) curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        else curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
    }
    if (selected.username_utf8) {
        *user = selected.username_utf8;
        if (curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, user->c_str()) != CURLE_OK) return false;
    }
    if (selected.password_utf8) {
        *password = selected.password_utf8;
        if (curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, password->c_str()) != CURLE_OK) return false;
    }
    return true;
}

void performHttp(const MaiVideoCircleOptions* options, const std::string& url,
                 const std::string& method, const std::string& body,
                 FILE* uploadFile, uint64_t uploadBytes,
                 const MaiVideoCircleCancel* cancel, MaiVideoCircleResult* result) {
    if (canceled(cancel)) {
        result->status = MAI_VIDEO_CIRCLE_CANCELED;
        return;
    }
    if (!initializeCurl(result)) return;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result->status = MAI_VIDEO_CIRCLE_NETWORK_ERROR;
        result->curl_code = CURLE_FAILED_INIT;
        return;
    }
    curl_slist* headers = nullptr;
    Response response;
    response.cancel = cancel;
    std::string proxy;
    std::string proxyUser;
    std::string proxyPassword;
    curl_blob caBlob{const_cast<char*>(options->ca_pem), options->ca_pem_length, CURL_BLOB_COPY};
    CURLcode setup = CURLE_OK;
    const auto set = [&](CURLoption key, auto value) {
        if (setup == CURLE_OK) setup = curl_easy_setopt(curl, key, value);
    };
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_PROTOCOLS_STR, "https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_NETRC, long(CURL_NETRC_IGNORED));
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_CONNECTTIMEOUT_MS,
        long(options->connect_timeout_ms > 0 ? options->connect_timeout_ms : 10'000));
    set(CURLOPT_TIMEOUT_MS, long(options->transfer_timeout_ms > 0
                                    ? options->transfer_timeout_ms
                                    : uploadFile ? 600'000 : 30'000));
    set(CURLOPT_LOW_SPEED_LIMIT, 1024L);
    set(CURLOPT_LOW_SPEED_TIME, 30L);
    set(CURLOPT_ACCEPT_ENCODING, "identity");
    set(CURLOPT_WRITEFUNCTION, receiveBody);
    set(CURLOPT_WRITEDATA, &response);
    set(CURLOPT_HEADERFUNCTION, receiveHeader);
    set(CURLOPT_HEADERDATA, &response);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, stopCanceled);
    set(CURLOPT_XFERINFODATA, &response);
    if (options->ca_pem_length > 0) set(CURLOPT_CAINFO_BLOB, &caBlob);
    else if (options->ca_path_utf8 && *options->ca_path_utf8)
        set(CURLOPT_CAINFO, options->ca_path_utf8);
#ifdef _WIN32
    else set(CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
#endif
    if (setup == CURLE_OK &&
        !configureProxy(curl, options, url, result, &proxy, &proxyUser, &proxyPassword))
        setup = CURLE_COULDNT_RESOLVE_PROXY;
    std::string authorization;
    if (uploadFile) {
        headers = curl_slist_append(headers, "Content-Type: video/mp4");
        if (headers) headers = curl_slist_append(headers, "x-oss-forbid-overwrite: true");
        if (headers) headers = curl_slist_append(headers, "Expect:");
        set(CURLOPT_UPLOAD, 1L);
        set(CURLOPT_READFUNCTION, readFile);
        set(CURLOPT_READDATA, uploadFile);
        set(CURLOPT_INFILESIZE_LARGE, curl_off_t(uploadBytes));
    } else {
        authorization = std::string("Authorization: Bearer ") + options->account_token_utf8;
        headers = curl_slist_append(headers, authorization.c_str());
        if (headers) headers = curl_slist_append(headers, "Accept: application/json");
        if (method == "POST") {
            if (headers) headers = curl_slist_append(headers, "Content-Type: application/json");
            set(CURLOPT_POST, 1L);
            set(CURLOPT_POSTFIELDS, body.c_str());
            set(CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
        } else {
            set(CURLOPT_HTTPGET, 1L);
        }
    }
    if (!headers && setup == CURLE_OK) setup = CURLE_OUT_OF_MEMORY;
    if (headers) set(CURLOPT_HTTPHEADER, headers);
    long httpStatus = 0;
    CURLcode code = setup;
    if (setup == CURLE_OK && !canceled(cancel)) {
        code = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    result->curl_code = int(code);
    result->http_status = httpStatus;
    if (canceled(cancel) || (code == CURLE_ABORTED_BY_CALLBACK && !response.tooLarge)) {
        result->status = MAI_VIDEO_CIRCLE_CANCELED;
        return;
    }
    if (response.tooLarge) {
        result->status = MAI_VIDEO_CIRCLE_RESPONSE_TOO_LARGE;
        return;
    }
    if (!response.body.empty() && !assignResponse(result, response.body)) {
        result->status = MAI_VIDEO_CIRCLE_NETWORK_ERROR;
        result->curl_code = CURLE_OUT_OF_MEMORY;
        return;
    }
    if (code != CURLE_OK) {
        result->status = MAI_VIDEO_CIRCLE_NETWORK_ERROR;
        return;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        result->status = MAI_VIDEO_CIRCLE_HTTP_ERROR;
        return;
    }
    if (!uploadFile) {
        const json responseJson = json::parse(response.body, nullptr, false);
        if (!responseJson.is_object()) {
            result->status = MAI_VIDEO_CIRCLE_INVALID_RESPONSE;
            return;
        }
    }
    result->status = MAI_VIDEO_CIRCLE_OK;
}

}  // namespace

extern "C" MaiVideoCircleCancel* mai_video_circle_cancel_create(void) {
    return new (std::nothrow) MaiVideoCircleCancel();
}

extern "C" void mai_video_circle_cancel_request(MaiVideoCircleCancel* cancel) {
    if (cancel) cancel->requested.store(true, std::memory_order_relaxed);
}

extern "C" void mai_video_circle_cancel_destroy(MaiVideoCircleCancel* cancel) {
    delete cancel;
}

extern "C" void mai_video_circle_result_free(MaiVideoCircleResult* result) {
    if (!result) return;
    std::free(result->response_json_utf8);
    *result = {MAI_VIDEO_CIRCLE_INVALID_ARGUMENT, 0, 0, nullptr, 0};
}

extern "C" void mai_video_circle_inspect_card(const char* completedCardJson,
                                                MaiVideoCircleResult* result) {
    if (!result) return;
    *result = {MAI_VIDEO_CIRCLE_INVALID_ARGUMENT, 0, 0, nullptr, 0};
    if (!completedCardJson || std::strlen(completedCardJson) > kMaxRequestBytes) return;
    const json card = json::parse(completedCardJson, nullptr, false);
    std::string sourceTool;
    std::string taskId;
    std::string mediaPath;
    if (!validCard(card, &sourceTool, &taskId, &mediaPath)) return;
    const std::string normalized = json{{"path", mediaPath}, {"source_tool", sourceTool},
                                        {"source_task_id", taskId}}.dump();
    if (!assignResponse(result, normalized)) {
        result->status = MAI_VIDEO_CIRCLE_NETWORK_ERROR;
        result->curl_code = CURLE_OUT_OF_MEMORY;
        return;
    }
    result->status = MAI_VIDEO_CIRCLE_OK;
}

extern "C" void mai_video_circle_request(const MaiVideoCircleOptions* options,
                                           MaiVideoCircleAction action,
                                           const char* payloadJson,
                                           const MaiVideoCircleCancel* cancel,
                                           MaiVideoCircleResult* result) {
    if (!result) return;
    *result = {MAI_VIDEO_CIRCLE_INVALID_ARGUMENT, 0, 0, nullptr, 0};
    if (!options || !options->account_token_utf8 ||
        !tokenChars(options->account_token_utf8) ||
        (options->ca_pem_length > 0 && !options->ca_pem) ||
        (options->ca_pem_length > 0 && options->ca_path_utf8 &&
         *options->ca_path_utf8)) return;
    const std::string inputText = payloadJson ? payloadJson : "{}";
    if (inputText.size() > kMaxRequestBytes) return;
    const json input = json::parse(inputText, nullptr, false);
    if (!input.is_object()) return;
    std::string authBase;
    std::string mediaBase;
    if (action == MAI_VIDEO_CIRCLE_SIGN_UPLOAD) {
        if (!validBase(options->media_base_url_utf8, &mediaBase)) return;
    } else if (!validBase(options->auth_base_url_utf8, &authBase)) return;
    std::string method;
    std::string url;
    std::string body;
    if (!buildRequest(action, input, authBase, mediaBase, &method, &url, &body) ||
        body.size() > kMaxRequestBytes) return;
    performHttp(options, url, method, body, nullptr, 0, cancel, result);
}

extern "C" void mai_video_circle_upload_file(const MaiVideoCircleOptions* options,
                                               const char* signedUploadJson,
                                               const char* filePath,
                                               uint64_t expectedBytes,
                                               const MaiVideoCircleCancel* cancel,
                                               MaiVideoCircleResult* result) {
    if (!result) return;
    *result = {MAI_VIDEO_CIRCLE_INVALID_ARGUMENT, 0, 0, nullptr, 0};
    if (!options || !signedUploadJson || !filePath || !*filePath ||
        std::strlen(signedUploadJson) > kMaxRequestBytes || expectedBytes < 1 ||
        expectedBytes > kMaxVideoBytes ||
        (options->ca_pem_length > 0 && !options->ca_pem) ||
        (options->ca_pem_length > 0 && options->ca_path_utf8 &&
         *options->ca_path_utf8)) return;
    const json signedUpload = json::parse(signedUploadJson, nullptr, false);
    std::string url;
    std::string objectKey;
    std::string contentType;
    std::string forbidOverwrite;
    if (!stringField(signedUpload, "upload_url", &url) ||
        !stringField(signedUpload, "object_key", &objectKey) ||
        !validObjectKey(objectKey) ||
        !signedUpload.contains("upload_headers") ||
        !stringField(signedUpload["upload_headers"], "Content-Type", &contentType) ||
        !stringField(signedUpload["upload_headers"], "x-oss-forbid-overwrite",
                     &forbidOverwrite) ||
        contentType != "video/mp4" || forbidOverwrite != "true" ||
        !validSignedUrl(url, objectKey, options->signed_upload_host_suffix_utf8)) return;
    std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(filePath), &std::fclose);
    if (!file) {
        result->status = MAI_VIDEO_CIRCLE_FILE_ERROR;
        return;
    }
    uint64_t actualBytes = 0;
    if (!fileSize(file.get(), &actualBytes) || actualBytes != expectedBytes) {
        result->status = MAI_VIDEO_CIRCLE_FILE_ERROR;
        return;
    }
    performHttp(options, url, "PUT", "", file.get(), expectedBytes, cancel, result);
}
