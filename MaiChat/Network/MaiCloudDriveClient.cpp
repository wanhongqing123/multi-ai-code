#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "MaiCloudDriveClient.h"
#include "MaiFirstPartyRouteInternal.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

struct MaiCloudDriveCancel {
    std::atomic_bool requested{false};
};

namespace {

using json = nlohmann::json;
constexpr uint64_t kMaxVideoBytes = 2'000'000'000;
constexpr uint64_t kMaxImageBytes = 20'000'000;
constexpr uint64_t kPartBytes = 8 * 1024 * 1024;
constexpr size_t kMaxJsonBytes = 1024 * 1024;
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxRequestBytes = 32 * 1024;

struct Response {
    std::string body;
    size_t headers = 0;
    bool tooLarge = false;
    bool progressCanceled = false;
    const MaiCloudDriveCancel* cancel = nullptr;
    MaiCloudDriveProgress progress = nullptr;
    void* progressContext = nullptr;
    uint64_t progressBase = 0;
    uint64_t progressTotal = 0;
    uint64_t partBytes = 0;
};

struct Part {
    uint32_t number = 0;
    uint64_t size = 0;
    std::string etag;
};

struct Checkpoint {
    std::string videoId;
    std::string uploadId;
    std::string fingerprint;
    std::string name;
    std::string contentType;
    std::string sourceId;
    std::string roomKey;
    uint64_t size = 0;
    uint64_t partSize = 0;
    uint32_t partsTotal = 0;
    bool completed = false;
};

bool canceled(const MaiCloudDriveCancel* cancel) {
    return cancel && cancel->requested.load(std::memory_order_relaxed);
}

bool printable(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x20 && c != 0x7f;
    });
}

bool validToken(const char* token) {
    if (!token) return false;
    const size_t length = std::strlen(token);
    return length > 0 && length <= 8192 &&
        std::all_of(token, token + length, [](unsigned char c) {
            return c >= 0x21 && c <= 0x7e;
        });
}

bool validUuid(const std::string& text) {
    if (text.size() != 36) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

bool nextUtf8CodePoint(const std::string& value, size_t* offset, uint32_t* output) {
    if (*offset >= value.size()) return false;
    const auto first = static_cast<unsigned char>(value[(*offset)++]);
    if (first < 0x80) { *output = first; return true; }
    unsigned remaining = 0;
    uint32_t code = 0;
    uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
        remaining = 1; code = first & 0x1f; minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        remaining = 2; code = first & 0x0f; minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        remaining = 3; code = first & 0x07; minimum = 0x10000;
    } else return false;
    if (remaining > value.size() - *offset) return false;
    while (remaining-- > 0) {
        const auto next = static_cast<unsigned char>(value[(*offset)++]);
        if ((next & 0xc0) != 0x80) return false;
        code = (code << 6) | (next & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return false;
    *output = code;
    return true;
}

bool unicodeWhitespace(uint32_t code) {
    // Match ECMAScript String.trim() used by the account metadata service.
    return (code >= 0x09 && code <= 0x0d) || code == 0x20 || code == 0xa0 ||
        code == 0x1680 || (code >= 0x2000 && code <= 0x200a) || code == 0x2028 ||
        code == 0x2029 || code == 0x202f || code == 0x205f || code == 0x3000 ||
        code == 0xfeff;
}

bool validFolderName(const std::string& name) {
    if (name.empty() || name.size() > 240 || name == "." || name == ".." ||
        name.back() == '.' || name.find_first_of("<>:\"|?*/\\") != std::string::npos)
        return false;
    size_t offset = 0;
    uint32_t first = 0;
    uint32_t last = 0;
    while (offset < name.size()) {
        const bool atStart = offset == 0;
        uint32_t code = 0;
        if (!nextUtf8CodePoint(name, &offset, &code) || code < 0x20 ||
            (code >= 0x7f && code <= 0x9f) || code == 0x2028 || code == 0x2029)
            return false;
        if (atStart) first = code;
        last = code;
    }
    if (unicodeWhitespace(first) || unicodeWhitespace(last)) return false;
    // NFC normalization and its final byte limit remain authoritative on the
    // service; platform clients do not each carry a separate Unicode database.
    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" &&
        !(stem.size() == 4 && (stem.compare(0, 3, "com") == 0 ||
                              stem.compare(0, 3, "lpt") == 0) &&
          stem[3] >= '1' && stem[3] <= '9');
}

bool stringField(const json& value, const char* name, std::string* output) {
    if (!value.is_object() || !value.contains(name) || !value[name].is_string())
        return false;
    *output = value[name].get<std::string>();
    return true;
}

bool uintField(const json& value, const char* name, uint64_t min, uint64_t max,
               uint64_t* output) {
    if (!value.is_object() || !value.contains(name) ||
        !value[name].is_number_integer() ||
        (!value[name].is_number_unsigned() && value[name].get<int64_t>() < 0))
        return false;
    const uint64_t number = value[name].get<uint64_t>();
    if (number < min || number > max) return false;
    *output = number;
    return true;
}

bool validFolder(const json& value) {
    std::string id;
    std::string name;
    uint64_t timestamp = 0;
    constexpr uint64_t maxSafeJsonInteger = 9'007'199'254'740'991;
    if (!value.is_object() || !stringField(value, "id", &id) || !validUuid(id) ||
        !stringField(value, "name", &name) || !validFolderName(name) ||
        !uintField(value, "created_at", 1, maxSafeJsonInteger, &timestamp) ||
        !uintField(value, "updated_at", 1, maxSafeJsonInteger, &timestamp) ||
        !value.contains("parent_id")) return false;
    return value["parent_id"].is_null() ||
        (value["parent_id"].is_string() &&
         validUuid(value["parent_id"].get<std::string>()));
}

bool mediaInfo(const std::string& name, MaiCloudDriveMediaInfo* output,
               bool session = false) {
    if (!output || name.size() < 3 || name.size() > 240 || !printable(name) ||
        name.front() == ' ' || name.back() == ' ' ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) return false;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    if (session) {
        if (name.find_first_of("<>:\"|?*") != std::string::npos ||
            name.back() == '.') return false;
        const std::string stem = lower.substr(0, lower.find('.'));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.rfind("com", 0) == 0 ||
                                  stem.rfind("lpt", 0) == 0) &&
             stem.back() >= '1' && stem.back() <= '9')) return false;
    }
    struct Format {
        const char* extension;
        const char* contentType;
        const char* mediaType;
        uint64_t maxBytes;
    };
    static constexpr Format formats[] = {
        {".mp4", "video/mp4", "video", kMaxVideoBytes},
        {".mov", "video/quicktime", "video", kMaxVideoBytes},
        {".jpg", "image/jpeg", "image", kMaxImageBytes},
        {".jpeg", "image/jpeg", "image", kMaxImageBytes},
        {".png", "image/png", "image", kMaxImageBytes},
        {".webp", "image/webp", "image", kMaxImageBytes},
        {".heic", "image/heic", "image", kMaxImageBytes},
    };
    for (const Format& format : formats) {
        const size_t length = std::strlen(format.extension);
        if (lower.size() > length &&
            lower.compare(lower.size() - length, length, format.extension) == 0) {
            *output = {format.contentType, format.mediaType, format.maxBytes};
            return true;
        }
    }
    if (session) {
        static constexpr Format extras[] = {
            {".gif", "image/gif", "image", kMaxImageBytes},
            {".mp3", "audio/mpeg", "audio", kMaxVideoBytes},
            {".m4a", "audio/mp4", "audio", kMaxVideoBytes},
            {".aac", "audio/aac", "audio", kMaxVideoBytes},
            {".wav", "audio/wav", "audio", kMaxVideoBytes},
            {".flac", "audio/flac", "audio", kMaxVideoBytes},
            {".ogg", "audio/ogg", "audio", kMaxVideoBytes},
        };
        for (const Format& format : extras) {
            const size_t length = std::strlen(format.extension);
            if (lower.size() > length &&
                lower.compare(lower.size() - length, length, format.extension) == 0) {
                *output = {format.contentType, format.mediaType, format.maxBytes};
                return true;
            }
        }
        const size_t dot = lower.rfind('.');
        if (dot != std::string::npos && dot > 0 &&
            lower.size() - dot - 1 <= 12 &&
            std::all_of(lower.begin() + dot + 1, lower.end(),
                        [](unsigned char c) { return std::isalnum(c); })) {
            *output = {"application/octet-stream", "file", kMaxVideoBytes};
            return true;
        }
    }
    return false;
}

bool validResponseMediaType(const json& response, const std::string& contentType) {
    const auto field = response.find("media_type");
    if (field == response.end()) return true; // Existing deployed video API.
    if (!field->is_string()) return false;
    const std::string actual = field->get<std::string>();
    return actual == (contentType.rfind("image/", 0) == 0 ? "image" :
        contentType.rfind("audio/", 0) == 0 ? "audio" :
        contentType.rfind("video/", 0) == 0 ? "video" : "file");
}

bool validHttpsUrl(const std::string& raw, bool queryRequired,
                   const char* hostSuffix) {
    if (raw.empty() || raw.size() > 16384 || !printable(raw)) return false;
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* query = nullptr;
    char* fragment = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, raw.c_str(), 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        scheme && std::strcmp(scheme, "https") == 0 &&
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK && host && *host &&
        (queryRequired ? curl_url_get(parsed, CURLUPART_QUERY, &query, 0) == CURLUE_OK &&
                         query && *query
                       : curl_url_get(parsed, CURLUPART_QUERY, &query, 0) != CURLUE_OK) &&
        curl_url_get(parsed, CURLUPART_FRAGMENT, &fragment, 0) != CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_USER, &user, 0) != CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK;
    bool hostMatches = true;
    if (hostSuffix) {
        const std::string suffix(hostSuffix);
        std::string lower = host ? host : "";
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        hostMatches = !suffix.empty() && suffix.size() <= 253 &&
            std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '.';
            }) &&
            (lower == suffix || (lower.size() > suffix.size() &&
                lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                lower[lower.size() - suffix.size() - 1] == '.'));
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(query);
    curl_free(fragment);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(parsed);
    return valid && hostMatches;
}

bool validBase(const char* raw, std::string* output) {
    if (!raw || std::strlen(raw) > 2048 || !validHttpsUrl(raw, false, nullptr) ||
        std::strstr(raw, "/../") || std::strstr(raw, "/./")) return false;
    *output = raw;
    while (output->size() > 8 && output->back() == '/') output->pop_back();
    return true;
}

bool validRoomID(const char* value) {
    if (!value) return false;
    const size_t length = std::strlen(value);
    return length > 0 && length <= 128 &&
        std::all_of(value, value + length, [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        });
}

bool sessionMode(const MaiCloudDriveOptions* options) {
    return options->session_id_utf8 != nullptr;
}

std::string apiRoot(const MaiCloudDriveOptions* options) {
    return sessionMode(options) ? "/v1/session-attachments" :
        "/v1/cloud-drive/videos";
}

std::string roomKey(const MaiCloudDriveOptions* options) {
    if (!sessionMode(options)) return {};
    return std::string(options->owner_device_slot_utf8) + ":" +
        options->session_id_utf8;
}

bool validOptions(const MaiCloudDriveOptions* options, bool accountRequired) {
    if (!options) return false;
    const bool hasRoom = options->owner_device_slot_utf8 ||
        options->owner_login_session_id_utf8 || options->session_id_utf8;
    if (hasRoom) {
        if (!options->owner_device_slot_utf8 ||
            !options->owner_login_session_id_utf8 ||
            !validRoomID(options->owner_login_session_id_utf8) ||
            !validRoomID(options->session_id_utf8)) return false;
        const std::string slot(options->owner_device_slot_utf8);
        if (slot != "ios_phone" && slot != "android_phone" &&
            slot != "tablet" && slot != "mac" && slot != "windows") return false;
    }
    return options && (!accountRequired || validToken(options->account_token_utf8)) &&
        !(options->ca_pem_length > 0 && !options->ca_pem) &&
        !(options->ca_pem_length > 0 && options->ca_path_utf8 &&
          *options->ca_path_utf8);
}

bool assignBody(MaiCloudDriveResult* result, const std::string& body) {
    if (body.empty()) return true;
    char* buffer = static_cast<char*>(std::malloc(body.size() + 1));
    if (!buffer) return false;
    std::memcpy(buffer, body.data(), body.size());
    buffer[body.size()] = '\0';
    result->response_json_utf8 = buffer;
    result->response_bytes = body.size();
    return true;
}

void resetResult(MaiCloudDriveResult* result) {
    *result = {MAI_CLOUD_DRIVE_INVALID_ARGUMENT, 0, 0, nullptr, 0};
}

bool initializeCurl(MaiCloudDriveResult* result) {
    static std::once_flag once;
    static CURLcode code = CURLE_FAILED_INIT;
    std::call_once(once, [] { code = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (code == CURLE_OK) return true;
    result->status = MAI_CLOUD_DRIVE_NETWORK_ERROR;
    result->curl_code = int(code);
    return false;
}

#ifdef _WIN32
std::wstring toWide(const char* utf8) {
    if (!utf8) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          utf8, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring wide(size_t(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                            wide.data(), count) != count) return {};
    wide.resize(size_t(count - 1));
    return wide;
}
#endif

FILE* openFile(const char* path, const char* mode) {
    if (!path || !*path) return nullptr;
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) return nullptr;
    std::wstring wideMode;
    for (const char* c = mode; *c; ++c) wideMode.push_back(wchar_t(*c));
    return _wfopen(wide.c_str(), wideMode.c_str());
#else
    return std::fopen(path, mode);
#endif
}

bool seekFile(FILE* file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, __int64(offset), SEEK_SET) == 0;
#else
    return fseeko(file, off_t(offset), SEEK_SET) == 0;
#endif
}

bool fileSize(FILE* file, uint64_t* output) {
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) return false;
    const auto length = _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END) != 0) return false;
    const auto length = ftello(file);
#endif
    if (length < 0 || !seekFile(file, 0)) return false;
    *output = uint64_t(length);
    return true;
}

MaiCloudDriveStatus readSource(const MaiCloudDriveSource* source,
                               uint64_t offset, unsigned char* output,
                               size_t bytes) {
    const int status = source->read_at(source->context, offset, output, bytes);
    return status == 1 ? MAI_CLOUD_DRIVE_OK :
           status == -1 ? MAI_CLOUD_DRIVE_SOURCE_NOT_SEEKABLE :
                          MAI_CLOUD_DRIVE_FILE_ERROR;
}

MaiCloudDriveStatus digestSource(const MaiCloudDriveSource* source,
                                 const MaiCloudDriveCancel* cancel,
                                 std::string* output) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                &EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        return MAI_CLOUD_DRIVE_FILE_ERROR;
    std::vector<unsigned char> chunk(1024 * 1024);
    uint64_t remaining = source->size_bytes;
    uint64_t offset = 0;
    while (remaining > 0) {
        if (canceled(cancel)) return MAI_CLOUD_DRIVE_CANCELED;
        const size_t count = size_t(std::min<uint64_t>(remaining, chunk.size()));
        const MaiCloudDriveStatus read = readSource(source, offset, chunk.data(), count);
        if (read != MAI_CLOUD_DRIVE_OK) return read;
        if (EVP_DigestUpdate(ctx.get(), chunk.data(), count) != 1)
            return MAI_CLOUD_DRIVE_FILE_ERROR;
        remaining -= count;
        offset += count;
    }
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLength = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash, &hashLength) != 1 || hashLength != 32)
        return MAI_CLOUD_DRIVE_FILE_ERROR;
    static constexpr char digits[] = "0123456789abcdef";
    output->resize(64);
    for (unsigned int i = 0; i < hashLength; ++i) {
        (*output)[2 * i] = digits[hash[i] >> 4];
        (*output)[2 * i + 1] = digits[hash[i] & 15];
    }
    return MAI_CLOUD_DRIVE_OK;
}

MaiCloudDriveStatus readPart(const MaiCloudDriveSource* source, uint64_t offset,
                             uint64_t size,
                             std::vector<unsigned char>* output) {
    if (size > kPartBytes) return MAI_CLOUD_DRIVE_FILE_ERROR;
    output->resize(size_t(size));
    return readSource(source, offset, output->data(), size_t(size));
}

int fileReadAt(void* context, uint64_t offset,
               unsigned char* output, size_t requested) {
    FILE* file = static_cast<FILE*>(context);
    if (!seekFile(file, offset)) return errno == ESPIPE ? -1 : 0;
    return std::fread(output, 1, requested, file) == requested ? 1 : 0;
}

bool contentMd5(const std::vector<unsigned char>& part, std::string* output) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(part.data(), part.size(), digest, &length,
                   EVP_md5(), nullptr) != 1 || length != 16) return false;
    unsigned char encoded[25]{};
    const int encodedLength = EVP_EncodeBlock(encoded, digest, int(length));
    if (encodedLength != 24) return false;
    output->assign(reinterpret_cast<char*>(encoded), size_t(encodedLength));
    return true;
}

bool writeCheckpoint(const char* path, const Checkpoint& value) {
    const json body{{"version", 1}, {"video_id", value.videoId},
        {"upload_id", value.uploadId}, {"file_sha256", value.fingerprint},
        {"file_size", value.size}, {"source_id", value.sourceId},
        {"name", value.name}, {"room_key", value.roomKey},
        {"content_type", value.contentType}, {"part_size", value.partSize},
        {"parts_total", value.partsTotal}, {"completed", value.completed}};
    const std::string serialized = body.dump();
    const std::string temp = std::string(path) + ".tmp";
    std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(temp.c_str(), "wb"),
                                                     &std::fclose);
    if (!file || std::fwrite(serialized.data(), 1, serialized.size(), file.get()) !=
                     serialized.size() || std::fflush(file.get()) != 0) return false;
#ifdef _WIN32
    if (_commit(_fileno(file.get())) != 0) return false;
#else
    if (fsync(fileno(file.get())) != 0) return false;
#endif
    file.reset();
#ifdef _WIN32
    const std::wstring source = toWide(temp.c_str());
    const std::wstring target = toWide(path);
    return !source.empty() && !target.empty() &&
        MoveFileExW(source.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(temp.c_str(), path) == 0;
#endif
}

bool readCheckpoint(const char* path, Checkpoint* output, bool* exists) {
    std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(path, "rb"), &std::fclose);
    if (!file) {
        *exists = errno != ENOENT;
        return !*exists;
    }
    *exists = true;
    char buffer[4096];
    const size_t count = std::fread(buffer, 1, sizeof(buffer), file.get());
    if (count == sizeof(buffer) || std::ferror(file.get())) return false;
    const json parsed = json::parse(std::string(buffer, count), nullptr, false);
    uint64_t version = 0;
    uint64_t partTotal = 0;
    if (!uintField(parsed, "version", 1, 1, &version) ||
        !stringField(parsed, "video_id", &output->videoId) ||
        !validUuid(output->videoId) ||
        !stringField(parsed, "upload_id", &output->uploadId) ||
        output->uploadId.empty() || output->uploadId.size() > 512 ||
        !stringField(parsed, "file_sha256", &output->fingerprint) ||
        output->fingerprint.size() != 64 ||
        !std::all_of(output->fingerprint.begin(), output->fingerprint.end(),
                     [](unsigned char c) {
                         return (c >= '0' && c <= '9') ||
                                (c >= 'a' && c <= 'f');
                     }) ||
        !stringField(parsed, "name", &output->name) ||
        !stringField(parsed, "source_id", &output->sourceId) ||
        output->sourceId.empty() || output->sourceId.size() > 4096 ||
        !stringField(parsed, "content_type", &output->contentType) ||
        !uintField(parsed, "file_size", 1, kMaxVideoBytes, &output->size) ||
        !uintField(parsed, "part_size", kPartBytes, kPartBytes, &output->partSize) ||
        !uintField(parsed, "parts_total", 1, 10000, &partTotal)) return false;
    const auto room = parsed.find("room_key");
    if (room != parsed.end() &&
        (!room->is_string() || room->get<std::string>().size() > 400)) return false;
    output->roomKey = room == parsed.end() ? "" : room->get<std::string>();
    MaiCloudDriveMediaInfo info{};
    if (!mediaInfo(output->name, &info, !output->roomKey.empty()) ||
        output->contentType != info.content_type_utf8 ||
        output->size > info.max_size_bytes)
        return false;
    output->partsTotal = uint32_t(partTotal);
    const auto completed = parsed.find("completed");
    if (completed != parsed.end()) {
        if (!completed->is_boolean()) return false;
        output->completed = completed->get<bool>();
    }
    return output->partsTotal == (output->size + kPartBytes - 1) / kPartBytes;
}

bool removeCheckpoint(const char* path) {
#ifdef _WIN32
    const std::wstring wide = toWide(path);
    if (wide.empty()) return false;
    return _wremove(wide.c_str()) == 0 || errno == ENOENT;
#else
    return std::remove(path) == 0 || errno == ENOENT;
#endif
}

size_t receiveBody(char* bytes, size_t size, size_t count, void* context) {
    auto* response = static_cast<Response*>(context);
    if (canceled(response->cancel)) return 0;
    if (size != 0 && count > SIZE_MAX / size) {
        response->tooLarge = true;
        return 0;
    }
    const size_t length = size * count;
    if (length > kMaxJsonBytes - response->body.size()) {
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
    if (length > kMaxHeaderBytes - response->headers) {
        response->tooLarge = true;
        return 0;
    }
    response->headers += length;
    return length;
}

int onProgress(void* context, curl_off_t, curl_off_t,
               curl_off_t, curl_off_t uploaded) {
    auto* response = static_cast<Response*>(context);
    if (canceled(response->cancel)) return 1;
    if (response->progress) {
        const uint64_t now = response->progressBase +
            std::min(response->partBytes,
                     uint64_t(std::max<curl_off_t>(uploaded, 0)));
        if (response->progress(now, response->progressTotal,
                               response->progressContext) != 0) {
            response->progressCanceled = true;
            return 1;
        }
    }
    return 0;
}

struct MemoryRead {
    const unsigned char* bytes = nullptr;
    size_t size = 0;
    size_t offset = 0;
};

size_t readMemory(char* output, size_t size, size_t count, void* context) {
    auto* input = static_cast<MemoryRead*>(context);
    if (size != 0 && count > SIZE_MAX / size) return CURL_READFUNC_ABORT;
    const size_t length = std::min(size * count, input->size - input->offset);
    if (length) std::memcpy(output, input->bytes + input->offset, length);
    input->offset += length;
    return length;
}

int seekMemory(void* context, curl_off_t offset, int origin) {
    auto* input = static_cast<MemoryRead*>(context);
    int64_t target = offset;
    if (origin == SEEK_CUR) target += int64_t(input->offset);
    else if (origin == SEEK_END) target += int64_t(input->size);
    else if (origin != SEEK_SET) return CURL_SEEKFUNC_FAIL;
    if (target < 0 || uint64_t(target) > input->size) return CURL_SEEKFUNC_FAIL;
    input->offset = size_t(target);
    return CURL_SEEKFUNC_OK;
}

bool configureProxy(CURL* curl, const MaiCloudDriveOptions* options,
                    const std::string& url, MaiCloudDriveResult* result,
                    std::string* proxy, std::string* user, std::string* password) {
    MaiDownloadProxy selected{options->proxy_url_utf8, options->proxy_username_utf8,
                              options->proxy_password_utf8};
    if (options->proxy_resolver &&
        options->proxy_resolver(url.c_str(), options->proxy_context, &selected) != 1) {
        result->status = MAI_CLOUD_DRIVE_NETWORK_ERROR;
        result->curl_code = CURLE_COULDNT_RESOLVE_PROXY;
        return false;
    }
    if (selected.url_utf8) {
        *proxy = selected.url_utf8;
        if (curl_easy_setopt(curl, CURLOPT_PROXY, proxy->c_str()) != CURLE_OK)
            return false;
        curl_easy_setopt(curl, CURLOPT_NOPROXY, proxy->empty() ? "*" : "");
    }
    if (selected.username_utf8) {
        *user = selected.username_utf8;
        if (curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, user->c_str()) != CURLE_OK)
            return false;
    }
    if (selected.password_utf8) {
        *password = selected.password_utf8;
        if (curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, password->c_str()) != CURLE_OK)
            return false;
    }
    return true;
}

void performHttp(const MaiCloudDriveOptions* options, const std::string& url,
                 const char* method, const std::string& body,
                 const char* contentMd5Header, const std::vector<unsigned char>* part,
                 const MaiCloudDriveCancel* cancel, MaiCloudDriveProgress progress,
                 void* progressContext, uint64_t progressBase, uint64_t progressTotal,
                 MaiCloudDriveResult* result, int defaultMetadataTimeoutMs = 30'000) {
    if (canceled(cancel)) {
        result->status = MAI_CLOUD_DRIVE_CANCELED;
        return;
    }
    if (!initializeCurl(result)) return;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result->status = MAI_CLOUD_DRIVE_NETWORK_ERROR;
        result->curl_code = CURLE_FAILED_INIT;
        return;
    }
    Response response;
    response.cancel = cancel;
    response.progress = progress;
    response.progressContext = progressContext;
    response.progressBase = progressBase;
    response.progressTotal = progressTotal;
    response.partBytes = part ? part->size() : 0;
    MemoryRead reader{part ? part->data() : nullptr, part ? part->size() : 0, 0};
    curl_slist* headers = nullptr;
    std::string proxy;
    std::string proxyUser;
    std::string proxyPassword;
    std::string authorization;
    std::string contentLength;
    std::string contentMd5;
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
    const long timeoutMs = options->transfer_timeout_ms > 0
        ? options->transfer_timeout_ms
        : part ? 600'000 : defaultMetadataTimeoutMs;
    set(CURLOPT_TIMEOUT_MS, timeoutMs);
    set(CURLOPT_LOW_SPEED_LIMIT, 1024L);
    // OSS CompleteMultipartUpload can be silent while it assembles the object.
    // Its total timeout, including an explicit caller override, must not be
    // cut short by the low-speed timer. Keep the 30-second stall limit for PUTs.
    set(CURLOPT_LOW_SPEED_TIME, part ? 30L
        : std::max(30L, timeoutMs / 1000 + (timeoutMs % 1000 != 0)));
    set(CURLOPT_ACCEPT_ENCODING, "identity");
    set(CURLOPT_WRITEFUNCTION, receiveBody);
    set(CURLOPT_WRITEDATA, &response);
    set(CURLOPT_HEADERFUNCTION, receiveHeader);
    set(CURLOPT_HEADERDATA, &response);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, onProgress);
    set(CURLOPT_XFERINFODATA, &response);
    if (options->ca_pem_length > 0) set(CURLOPT_CAINFO_BLOB, &caBlob);
    else if (options->ca_path_utf8 && *options->ca_path_utf8)
        set(CURLOPT_CAINFO, options->ca_path_utf8);
#ifdef _WIN32
    else set(CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
#endif
    // Select before sending any business request. A signed OSS part must not
    // inherit the account endpoint's DNS or proxy route.
    const bool routed = options->first_party_route_selector &&
        maiFirstPartyRoute::eligible(url, part == nullptr);
    int route = MAI_FIRST_PARTY_ROUTE_SYSTEM;
    if (setup == CURLE_OK && routed)
        route = options->first_party_route_selector(url.c_str(), options->ca_path_utf8,
                                                    options->first_party_route_context);
    if (setup == CURLE_OK && route != MAI_FIRST_PARTY_ROUTE_SYSTEM)
        setup = maiFirstPartyRoute::apply(curl, route);
    else if (setup == CURLE_OK &&
             !configureProxy(curl, options, url, result, &proxy, &proxyUser, &proxyPassword))
        setup = CURLE_COULDNT_RESOLVE_PROXY;
    if (part) {
        // Signed OSS URL: deliberately omit account Authorization and cookies.
        contentLength = "Content-Length: " + std::to_string(part->size());
        contentMd5 = std::string("Content-MD5: ") + contentMd5Header;
        headers = curl_slist_append(headers, contentLength.c_str());
        if (headers) headers = curl_slist_append(headers, contentMd5.c_str());
        if (headers) headers = curl_slist_append(headers, "Expect:");
        set(CURLOPT_UPLOAD, 1L);
        set(CURLOPT_READFUNCTION, readMemory);
        set(CURLOPT_READDATA, &reader);
        set(CURLOPT_SEEKFUNCTION, seekMemory);
        set(CURLOPT_SEEKDATA, &reader);
        set(CURLOPT_INFILESIZE_LARGE, curl_off_t(part->size()));
    } else {
        authorization = std::string("Authorization: Bearer ") + options->account_token_utf8;
        headers = curl_slist_append(headers, authorization.c_str());
        if (headers) headers = curl_slist_append(headers, "Accept: application/json");
        if (std::strcmp(method, "POST") == 0 || std::strcmp(method, "PATCH") == 0) {
            if (headers) headers = curl_slist_append(headers, "Content-Type: application/json");
            set(CURLOPT_POST, 1L);
            set(CURLOPT_POSTFIELDS, body.c_str());
            set(CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
            if (std::strcmp(method, "PATCH") == 0) set(CURLOPT_CUSTOMREQUEST, "PATCH");
        } else if (std::strcmp(method, "DELETE") == 0) {
            set(CURLOPT_CUSTOMREQUEST, "DELETE");
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
        // The server may already have applied a write. Invalidate only for the
        // next operation; never replay this request after an ambiguous failure.
        if (routed && code != CURLE_OK && !canceled(cancel) &&
            options->first_party_route_invalidator)
            options->first_party_route_invalidator(options->first_party_route_context);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    result->curl_code = int(code);
    result->http_status = httpStatus;
    if (canceled(cancel) || response.progressCanceled ||
        (code == CURLE_ABORTED_BY_CALLBACK && !response.tooLarge)) {
        result->status = MAI_CLOUD_DRIVE_CANCELED;
        return;
    }
    if (response.tooLarge) {
        result->status = MAI_CLOUD_DRIVE_RESPONSE_TOO_LARGE;
        return;
    }
    if (!assignBody(result, response.body)) {
        result->status = MAI_CLOUD_DRIVE_NETWORK_ERROR;
        result->curl_code = CURLE_OUT_OF_MEMORY;
        return;
    }
    if (code != CURLE_OK) {
        result->status = MAI_CLOUD_DRIVE_NETWORK_ERROR;
        return;
    }
    result->status = httpStatus >= 200 && httpStatus < 300
        ? MAI_CLOUD_DRIVE_OK : MAI_CLOUD_DRIVE_HTTP_ERROR;
}

void requestInternal(const MaiCloudDriveOptions* options, const std::string& base,
                     const std::string& route, const char* method,
                     const json& body, const MaiCloudDriveCancel* cancel,
                     MaiCloudDriveResult* result,
                     int defaultMetadataTimeoutMs = 30'000) {
    resetResult(result);
    const bool hasBody = std::strcmp(method, "POST") == 0 || std::strcmp(method, "PATCH") == 0;
    const std::string encoded = hasBody ? body.dump() : "";
    if (encoded.size() > kMaxRequestBytes) return;
    performHttp(options, base + route, method, encoded, nullptr, nullptr,
                cancel, nullptr, nullptr, 0, 0, result,
                defaultMetadataTimeoutMs);
    if (result->status == MAI_CLOUD_DRIVE_OK) {
        const json parsed = json::parse(result->response_json_utf8
            ? std::string(result->response_json_utf8, result->response_bytes) : "",
            nullptr, false);
        if (!parsed.is_object()) result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    }
}

void moveResult(MaiCloudDriveResult* destination, MaiCloudDriveResult* source) {
    *destination = *source;
    resetResult(source);
}

bool parseInitial(const json& response, uint64_t fileBytes,
                  const std::string& name, const std::string& contentType,
                  const std::string& fingerprint, Checkpoint* output) {
    uint64_t partSize = 0;
    uint64_t partCount = 0;
    std::string expiry;
    if (!stringField(response, "video_id", &output->videoId) ||
        !validUuid(output->videoId) ||
        !validResponseMediaType(response, contentType) ||
        !stringField(response, "upload_id", &output->uploadId) ||
        output->uploadId.empty() || output->uploadId.size() > 512 ||
        !uintField(response, "part_size", kPartBytes, kPartBytes, &partSize) ||
        !uintField(response, "parts_total", 1, 10000, &partCount) ||
        !stringField(response, "upload_expires_at", &expiry) || expiry.empty() ||
        partCount != (fileBytes + kPartBytes - 1) / kPartBytes) return false;
    output->size = fileBytes;
    output->partSize = partSize;
    output->partsTotal = uint32_t(partCount);
    output->name = name;
    output->contentType = contentType;
    output->fingerprint = fingerprint;
    return true;
}

uint64_t expectedPartBytes(const Checkpoint& checkpoint, uint32_t number) {
    if (number < 1 || number > checkpoint.partsTotal) return 0;
    return std::min<uint64_t>(checkpoint.partSize,
        checkpoint.size - uint64_t(number - 1) * checkpoint.partSize);
}

bool parseParts(const json& response, const Checkpoint& checkpoint,
                std::vector<Part>* parts, bool* alreadyReady) {
    std::string videoId;
    std::string status;
    if (!stringField(response, "video_id", &videoId) ||
        videoId != checkpoint.videoId ||
        !stringField(response, "status", &status)) return false;
    // A completion request may have reached OSS while its response was lost.
    // Reissuing the idempotent complete call settles either state.
    *alreadyReady = status == "ready" || status == "completing";
    if (*alreadyReady) return true;
    uint64_t partSize = 0;
    uint64_t partCount = 0;
    if (status != "uploading" ||
        !uintField(response, "part_size", kPartBytes, kPartBytes, &partSize) ||
        !uintField(response, "parts_total", 1, 10000, &partCount) ||
        partSize != checkpoint.partSize || partCount != checkpoint.partsTotal ||
        !response.contains("parts") || !response["parts"].is_array() ||
        response["parts"].size() > checkpoint.partsTotal) return false;
    parts->clear();
    std::vector<bool> seen(checkpoint.partsTotal + 1, false);
    for (const json& entry : response["parts"]) {
        uint64_t number = 0;
        uint64_t bytes = 0;
        std::string etag;
        if (!uintField(entry, "part_number", 1, checkpoint.partsTotal, &number) ||
            !uintField(entry, "size_bytes", 1, kPartBytes, &bytes) ||
            !stringField(entry, "etag", &etag) || etag.empty() ||
            etag.size() > 512 || seen[size_t(number)] ||
            bytes != expectedPartBytes(checkpoint, uint32_t(number))) return false;
        seen[size_t(number)] = true;
        parts->push_back({uint32_t(number), bytes, std::move(etag)});
    }
    return true;
}

bool parsePartSign(const json& response, const MaiCloudDriveOptions* options,
                   uint32_t number, uint64_t bytes, const std::string& md5,
                   std::string* url) {
    uint64_t signedNumber = 0;
    uint64_t signedBytes = 0;
    uint64_t expires = 0;
    std::string lengthHeader;
    std::string md5Header;
    if (!uintField(response, "part_number", number, number, &signedNumber) ||
        !uintField(response, "part_size_bytes", bytes, bytes, &signedBytes) ||
        !uintField(response, "expires_in", 1, 3600, &expires) ||
        !stringField(response, "upload_url", url) ||
        !validHttpsUrl(*url, true, options->signed_upload_host_suffix_utf8
                                      ? options->signed_upload_host_suffix_utf8
                                      : "aliyuncs.com") ||
        !response.contains("upload_headers") ||
        !stringField(response["upload_headers"], "Content-Length", &lengthHeader) ||
        !stringField(response["upload_headers"], "Content-MD5", &md5Header) ||
        lengthHeader != std::to_string(bytes) || md5Header != md5) return false;
    return true;
}

bool parseCompletion(const json& response, const Checkpoint& checkpoint) {
    std::string id;
    std::string status;
    std::string name;
    std::string contentType;
    uint64_t bytes = 0;
    return stringField(response, "video_id", &id) && id == checkpoint.videoId &&
        stringField(response, "status", &status) && status == "ready" &&
        validResponseMediaType(response, checkpoint.contentType) &&
        stringField(response, "name", &name) && name == checkpoint.name &&
        stringField(response, "content_type", &contentType) &&
        contentType == checkpoint.contentType &&
        uintField(response, "size_bytes", checkpoint.size, checkpoint.size, &bytes);
}

json parseResponse(const MaiCloudDriveResult& result) {
    return json::parse(result.response_json_utf8
        ? std::string(result.response_json_utf8, result.response_bytes) : "",
        nullptr, false);
}

bool pausedWait(const MaiCloudDriveCancel* cancel, unsigned milliseconds) {
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 50) {
        if (canceled(cancel)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !canceled(cancel);
}

struct MonotonicProgress {
    MaiCloudDriveProgress callback = nullptr;
    void* context = nullptr;
    uint64_t highWater = 0;
};

int onMonotonicProgress(uint64_t uploaded, uint64_t total, void* context) {
    auto* progress = static_cast<MonotonicProgress*>(context);
    progress->highWater = std::max(progress->highWater, uploaded);
    return progress->callback(progress->highWater, total, progress->context);
}

void uploadPart(const MaiCloudDriveOptions* options, const std::string& base,
                const MaiCloudDriveSource* source,
                const Checkpoint& checkpoint, uint32_t number,
                uint64_t confirmedBytes, const MaiCloudDriveCancel* cancel,
                MaiCloudDriveProgress progress, void* progressContext,
                MaiCloudDriveResult* result) {
    resetResult(result);
    const uint64_t bytes = expectedPartBytes(checkpoint, number);
    std::vector<unsigned char> contents;
    result->status = readPart(source,
        uint64_t(number - 1) * checkpoint.partSize, bytes, &contents);
    if (result->status != MAI_CLOUD_DRIVE_OK) {
        return;
    }
    std::string md5;
    if (!contentMd5(contents, &md5)) {
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
        return;
    }
    const std::string route = apiRoot(options) + "/uploads/" +
        checkpoint.videoId + "/parts/" + std::to_string(number) + "/sign";
    MonotonicProgress monotonic{progress, progressContext, confirmedBytes};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (canceled(cancel)) {
            result->status = MAI_CLOUD_DRIVE_CANCELED;
            return;
        }
        MaiCloudDriveResult signedResult{};
        requestInternal(options, base, route, "POST",
                        json{{"content_md5_base64", md5}}, cancel, &signedResult);
        if (signedResult.status != MAI_CLOUD_DRIVE_OK) {
            moveResult(result, &signedResult);
        } else {
            std::string url;
            if (!parsePartSign(parseResponse(signedResult), options,
                               number, bytes, md5, &url)) {
                result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
                mai_cloud_drive_result_free(&signedResult);
                return;
            }
            mai_cloud_drive_result_free(&signedResult);
            performHttp(options, url, "PUT", "", md5.c_str(), &contents,
                        cancel, progress ? onMonotonicProgress : nullptr,
                        progress ? &monotonic : nullptr,
                        confirmedBytes, checkpoint.size, result);
        }
        if (result->status == MAI_CLOUD_DRIVE_OK ||
            result->status == MAI_CLOUD_DRIVE_CANCELED) return;
        const bool retry = result->status == MAI_CLOUD_DRIVE_NETWORK_ERROR ||
            (result->status == MAI_CLOUD_DRIVE_HTTP_ERROR &&
             (result->http_status == 403 || result->http_status == 408 ||
              result->http_status == 429 || result->http_status >= 500));
        if (!retry || attempt == 2) return;
        mai_cloud_drive_result_free(result);
        if (!pausedWait(cancel, 250U * (attempt + 1))) {
            result->status = MAI_CLOUD_DRIVE_CANCELED;
            return;
        }
    }
}

void createSession(const MaiCloudDriveOptions* options, const std::string& base,
                   const char* checkpointPath, uint64_t bytes,
                   const char* sourceId,
                   const std::string& name, const std::string& contentType,
                   const std::string& fingerprint,
                   const MaiCloudDriveCancel* cancel, Checkpoint* checkpoint,
                   MaiCloudDriveResult* result) {
    MaiCloudDriveResult initial{};
    json request{{"name", name}, {"size_bytes", bytes},
                 {"content_type", contentType}};
    if (sessionMode(options)) {
        request["owner_slot"] = options->owner_device_slot_utf8;
        request["owner_login_session_id"] = options->owner_login_session_id_utf8;
        request["session_id"] = options->session_id_utf8;
    }
    requestInternal(options, base, apiRoot(options) + "/uploads", "POST",
        request, cancel, &initial);
    if (initial.status != MAI_CLOUD_DRIVE_OK) {
        moveResult(result, &initial);
        return;
    }
    const json response = parseResponse(initial);
    std::string returnedRoom;
    if (!parseInitial(response, bytes, name, contentType,
                      fingerprint, checkpoint) ||
        (sessionMode(options) &&
         (!stringField(response, "room_key", &returnedRoom) ||
          returnedRoom != roomKey(options)))) {
        result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
        mai_cloud_drive_result_free(&initial);
        return;
    }
    checkpoint->sourceId = sourceId;
    checkpoint->roomKey = roomKey(options);
    mai_cloud_drive_result_free(&initial);
    if (writeCheckpoint(checkpointPath, *checkpoint)) {
        result->status = MAI_CLOUD_DRIVE_OK;
        return;
    }
    // A session without a durable checkpoint cannot be resumed. Release its
    // reservation; expiry cleanup remains a fallback if this request fails.
    MaiCloudDriveResult aborted{};
    requestInternal(options, base,
        apiRoot(options) + "/uploads/" + checkpoint->videoId,
        "DELETE", json::object(), cancel, &aborted);
    mai_cloud_drive_result_free(&aborted);
    result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
}

bool listParts(const MaiCloudDriveOptions* options, const std::string& base,
               const Checkpoint& checkpoint, const MaiCloudDriveCancel* cancel,
               std::vector<Part>* parts, bool* ready,
               MaiCloudDriveResult* result) {
    MaiCloudDriveResult listed{};
    requestInternal(options, base,
        apiRoot(options) + "/uploads/" + checkpoint.videoId + "/parts",
        "GET", json::object(), cancel, &listed);
    if (listed.status != MAI_CLOUD_DRIVE_OK) {
        moveResult(result, &listed);
        return false;
    }
    if (!parseParts(parseResponse(listed), checkpoint, parts, ready)) {
        result->status = MAI_CLOUD_DRIVE_PART_MISMATCH;
        mai_cloud_drive_result_free(&listed);
        return false;
    }
    mai_cloud_drive_result_free(&listed);
    return true;
}

bool completeUpload(const MaiCloudDriveOptions* options, const std::string& base,
                    const Checkpoint& checkpoint, const char* checkpointPath,
                    const MaiCloudDriveCancel* cancel, MaiCloudDriveResult* result) {
    MaiCloudDriveResult completed{};
    requestInternal(options, base,
        apiRoot(options) + "/uploads/" + checkpoint.videoId + "/complete",
        "POST", json::object(), cancel, &completed, 200'000);
    if (completed.status != MAI_CLOUD_DRIVE_OK) {
        moveResult(result, &completed);
        return false;
    }
    if (!parseCompletion(parseResponse(completed), checkpoint)) {
        result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
        mai_cloud_drive_result_free(&completed);
        return false;
    }
    moveResult(result, &completed);
    if (options->retain_completed_checkpoint) {
        Checkpoint receipt = checkpoint;
        receipt.completed = true;
        if (!writeCheckpoint(checkpointPath, receipt))
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
    } else if (!removeCheckpoint(checkpointPath))
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
    return true;
}

}  // namespace

extern "C" int mai_cloud_drive_media_info(const char* displayName,
                                           MaiCloudDriveMediaInfo* output) {
    if (!displayName || !output || std::strlen(displayName) > 240) return 0;
    *output = {};
    return mediaInfo(displayName, output) ? 1 : 0;
}

extern "C" int mai_cloud_drive_folder_name_valid(const char* name) {
    return name && std::strlen(name) <= 240 && validFolderName(name) ? 1 : 0;
}

extern "C" int mai_session_attachment_media_info(const char* displayName,
                                                    MaiCloudDriveMediaInfo* output) {
    if (!displayName) return 0;
    return mediaInfo(displayName, output, true) ? 1 : 0;
}

extern "C" MaiCloudDriveCancel* mai_cloud_drive_cancel_create(void) {
    return new (std::nothrow) MaiCloudDriveCancel();
}

extern "C" void mai_cloud_drive_cancel_request(MaiCloudDriveCancel* cancel) {
    if (cancel) cancel->requested.store(true, std::memory_order_relaxed);
}

extern "C" void mai_cloud_drive_cancel_destroy(MaiCloudDriveCancel* cancel) {
    delete cancel;
}

extern "C" void mai_cloud_drive_result_free(MaiCloudDriveResult* result) {
    if (!result) return;
    std::free(result->response_json_utf8);
    resetResult(result);
}

extern "C" void mai_cloud_drive_request(const MaiCloudDriveOptions* options,
                                          MaiCloudDriveAction action,
                                          const char* payloadJson,
                                          const MaiCloudDriveCancel* cancel,
                                          MaiCloudDriveResult* result) {
    if (!result) return;
    resetResult(result);
    if (!validOptions(options, true)) return;
    std::string base;
    if (!validBase(options->auth_base_url_utf8, &base)) return;
    const std::string payload = payloadJson ? payloadJson : "{}";
    if (payload.size() > kMaxRequestBytes) return;
    const json input = json::parse(payload, nullptr, false);
    if (!input.is_object()) return;
    const std::string root = apiRoot(options);
    std::string route;
    const char* method = "GET";
    json request = json::object();
    const bool folderAction = action >= MAI_CLOUD_DRIVE_LIST_FOLDERS &&
        action <= MAI_CLOUD_DRIVE_RENAME_MEDIA;
    if (folderAction && sessionMode(options)) return;
    if (action == MAI_SESSION_ATTACHMENT_CLEANUP_ROOM && sessionMode(options)) {
        route = root + "/cleanup-room";
        method = "POST";
        request = {{"owner_slot", options->owner_device_slot_utf8},
                   {"owner_login_session_id", options->owner_login_session_id_utf8},
                   {"session_id", options->session_id_utf8}};
    } else if (action == MAI_CLOUD_DRIVE_LIST_VIDEOS) {
        route = root;
        if (input.contains("before")) {
            std::string cursor;
            if (!stringField(input, "before", &cursor) || cursor.empty() ||
                cursor.size() > 256 ||
                !std::all_of(cursor.begin(), cursor.end(), [](unsigned char c) {
                    return std::isalnum(c) || c == '_' || c == '-';
                })) return;
            route += "?before=" + cursor;
        }
        if (input.contains("folder_id")) {
            std::string folder;
            if (sessionMode(options) || !stringField(input, "folder_id", &folder) ||
                (!folder.empty() && !validUuid(folder))) return;
            route += std::string(route.find('?') == std::string::npos ? "?" : "&") +
                "folder_id=" + folder;
        }
    } else if (folderAction) {
        const std::string folders = "/v1/cloud-drive/folders";
        if (action == MAI_CLOUD_DRIVE_LIST_FOLDERS) {
            if (!input.empty()) return;
            route = folders;
        } else if (action == MAI_CLOUD_DRIVE_MOVE_MEDIA || action == MAI_CLOUD_DRIVE_RENAME_MEDIA) {
            std::string id;
            if (input.size() != 2 || !stringField(input, "media_id", &id) || !validUuid(id)) return;
            if (action == MAI_CLOUD_DRIVE_RENAME_MEDIA) {
                std::string name;
                MaiCloudDriveMediaInfo media{};
                if (!stringField(input, "name", &name) || !validFolderName(name) ||
                    !mediaInfo(name, &media)) return;
                request["name"] = name;
            } else {
                std::string folder;
                if (!stringField(input, "folder_id", &folder) ||
                    (!folder.empty() && !validUuid(folder))) return;
                request["folder_id"] = folder;
            }
            route = root + "/" + id;
            method = "PATCH";
        } else {
            const bool create = action == MAI_CLOUD_DRIVE_CREATE_FOLDER;
            std::string id;
            if (!create && (!stringField(input, "folder_id", &id) || !validUuid(id)))
                return;
            route = folders + (create ? "" : "/" + id);
            if (action == MAI_CLOUD_DRIVE_DELETE_FOLDER) {
                if (input.size() != 1) return;
                method = "DELETE";
            } else {
                method = create ? "POST" : "PATCH";
                if (create && !input.contains("name")) return;
                if (!create && !input.contains("name") && !input.contains("parent_id")) return;
                for (auto it = input.begin(); it != input.end(); ++it) {
                    if (!create && it.key() == "folder_id") continue;
                    if (it.key() == "name") {
                        if (!it.value().is_string() ||
                            !validFolderName(it.value().get<std::string>())) return;
                    } else if (it.key() == "parent_id") {
                        if (!it.value().is_string()) return;
                        const std::string parent = it.value().get<std::string>();
                        if (!parent.empty() && !validUuid(parent)) return;
                    } else return;
                    request[it.key()] = it.value();
                }
            }
        }
    } else {
        std::string id;
        if (!stringField(input, "video_id", &id) || !validUuid(id)) return;
        if (action == MAI_CLOUD_DRIVE_PLAY_LINK) route = root + "/" + id + "/play-link";
        else if (action == MAI_CLOUD_DRIVE_THUMBNAIL_LINK)
            route = root + "/" + id + "/thumbnail-link";
        else if (action == MAI_CLOUD_DRIVE_DELETE_VIDEO) {
            route = root + "/" + id;
            method = "DELETE";
        } else if (action == MAI_CLOUD_DRIVE_ABORT_UPLOAD) {
            route = root + "/uploads/" + id;
            method = "DELETE";
        } else return;
    }
    requestInternal(options, base, route, method, request, cancel, result);
    if (result->status != MAI_CLOUD_DRIVE_OK) return;
    const json response = parseResponse(*result);
    std::string field;
    if (action == MAI_CLOUD_DRIVE_LIST_VIDEOS) {
        if (!response.contains("videos") || !response["videos"].is_array() ||
            !response.contains("next_cursor") ||
            !(response["next_cursor"].is_null() ||
              response["next_cursor"].is_string()) ||
            !response.contains("quota") || !response["quota"].is_object())
            result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    } else if (action == MAI_CLOUD_DRIVE_LIST_FOLDERS) {
        if (!response.is_object() || !response.contains("folders") ||
            !response["folders"].is_array() || response["folders"].size() > 1000 ||
            !std::all_of(response["folders"].begin(), response["folders"].end(),
                         [](const json& folder) { return validFolder(folder); }))
            result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    } else if (action == MAI_CLOUD_DRIVE_CREATE_FOLDER ||
               action == MAI_CLOUD_DRIVE_UPDATE_FOLDER) {
        if (!validFolder(response) ||
            (action == MAI_CLOUD_DRIVE_UPDATE_FOLDER &&
             response["id"] != input["folder_id"]))
            result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    } else if (action == MAI_CLOUD_DRIVE_DELETE_FOLDER) {
        std::string status;
        if (!stringField(response, "folder_id", &field) || field != input["folder_id"] ||
            !stringField(response, "status", &status) || status != "deleted")
            result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    } else if (action == MAI_CLOUD_DRIVE_MOVE_MEDIA || action == MAI_CLOUD_DRIVE_RENAME_MEDIA) {
        std::string status;
        std::string name;
        std::string contentType;
        std::string mediaType;
        uint64_t number = 0;
        MaiCloudDriveMediaInfo media{};
        if (!stringField(response, "video_id", &field) || field != input["media_id"] ||
            !stringField(response, "status", &status) || status != "ready" ||
            !stringField(response, "name", &name) || !mediaInfo(name, &media) ||
            !stringField(response, "content_type", &contentType) ||
            contentType != media.content_type_utf8 ||
            !stringField(response, "media_type", &mediaType) || mediaType != media.media_type_utf8 ||
            !uintField(response, "size_bytes", 1, media.max_size_bytes, &number) ||
            !uintField(response, "created_at", 1, 9'007'199'254'740'991, &number) ||
            !response.contains("folder_id") ||
            !(response["folder_id"].is_null() || (response["folder_id"].is_string() &&
              validUuid(response["folder_id"].get<std::string>()))) ||
            (action == MAI_CLOUD_DRIVE_MOVE_MEDIA &&
             (input["folder_id"] == "" ? !response["folder_id"].is_null() :
              response["folder_id"] != input["folder_id"])))
            result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
        if (result->status == MAI_CLOUD_DRIVE_OK && action == MAI_CLOUD_DRIVE_RENAME_MEDIA) {
            const std::string requestedName = input["name"].get<std::string>();
            std::string returnedExtension = name.substr(name.find_last_of('.'));
            std::string requestedExtension = requestedName.substr(requestedName.find_last_of('.'));
            std::transform(returnedExtension.begin(), returnedExtension.end(), returnedExtension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(requestedExtension.begin(), requestedExtension.end(), requestedExtension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!validFolderName(name) || returnedExtension != requestedExtension)
                result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
        }
    } else if (action == MAI_CLOUD_DRIVE_PLAY_LINK ||
               action == MAI_CLOUD_DRIVE_THUMBNAIL_LINK) {
        uint64_t expires = 0;
        const bool thumbnail = action == MAI_CLOUD_DRIVE_THUMBNAIL_LINK;
        if (!stringField(response, thumbnail ? "thumbnail_url" : "read_url", &field) ||
            !validHttpsUrl(field, true,
                thumbnail ? (options->signed_upload_host_suffix_utf8
                    ? options->signed_upload_host_suffix_utf8 : "aliyuncs.com") : nullptr) ||
            !uintField(response,
                thumbnail ? "thumbnail_expires_in" : "read_expires_in",
                1, 3600, &expires) ||
            (thumbnail && (!stringField(response, "content_type", &field) ||
                           field != "image/jpeg")))
            result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    } else if (action == MAI_SESSION_ATTACHMENT_CLEANUP_ROOM) {
        if (!stringField(response, "room_key", &field) ||
            field != roomKey(options)) result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    } else if (!stringField(response, "video_id", &field) ||
               !validUuid(field)) {
        result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
    }
}

extern "C" void mai_cloud_drive_upload_resumable_source(
    const MaiCloudDriveOptions* options, const MaiCloudDriveSource* source,
    const char* checkpointPath, const char* displayName,
    const char* contentTypeRaw, const MaiCloudDriveCancel* cancel,
    MaiCloudDriveProgress progress, void* progressContext,
    MaiCloudDriveResult* result) {
    if (!result) return;
    resetResult(result);
    if (!validOptions(options, true) || !source || !source->read_at ||
        !source->stable_id_utf8 || !*source->stable_id_utf8 ||
        std::strlen(source->stable_id_utf8) > 4096 ||
        !printable(source->stable_id_utf8) ||
        !checkpointPath || !*checkpointPath ||
        std::strlen(checkpointPath) > 4096 ||
        !displayName || !contentTypeRaw) return;
    try {
        if (!std::filesystem::u8path(checkpointPath).is_absolute()) return;
    } catch (...) { return; }
    const std::string name(displayName);
    const std::string contentType(contentTypeRaw);
    MaiCloudDriveMediaInfo info{};
    if (!mediaInfo(name, &info, sessionMode(options)) ||
        contentType != info.content_type_utf8) return;
    std::string base;
    if (!validBase(options->auth_base_url_utf8, &base)) return;
    const uint64_t bytes = source->size_bytes;
    if (bytes == 0 || bytes > info.max_size_bytes) return;
    std::string fingerprint;
    result->status = digestSource(source, cancel, &fingerprint);
    if (result->status != MAI_CLOUD_DRIVE_OK) return;
    Checkpoint checkpoint;
    bool exists = false;
    if (!readCheckpoint(checkpointPath, &checkpoint, &exists)) {
        result->status = MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH;
        return;
    }
    if (exists && (checkpoint.size != bytes ||
                   checkpoint.fingerprint != fingerprint ||
                   checkpoint.name != name ||
                   checkpoint.contentType != contentType ||
                   checkpoint.roomKey != roomKey(options))) {
        result->status = MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH;
        return;
    }
    if (exists && checkpoint.sourceId != source->stable_id_utf8) {
        checkpoint.sourceId = source->stable_id_utf8;
        if (!writeCheckpoint(checkpointPath, checkpoint)) {
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
            return;
        }
    }
    std::vector<Part> parts;
    bool ready = false;
    for (unsigned sessionAttempt = 0; sessionAttempt < 2; ++sessionAttempt) {
        if (!exists) {
            createSession(options, base, checkpointPath, bytes,
                          source->stable_id_utf8, name,
                          contentType, fingerprint, cancel, &checkpoint, result);
            if (result->status != MAI_CLOUD_DRIVE_OK) return;
        }
        if (listParts(options, base, checkpoint, cancel, &parts, &ready, result))
            break;
        if (result->status == MAI_CLOUD_DRIVE_HTTP_ERROR &&
            result->http_status == 410 && sessionAttempt == 0) {
            mai_cloud_drive_result_free(result);
            if (!removeCheckpoint(checkpointPath)) {
                result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
                return;
            }
            exists = false;
            continue;
        }
        return;
    }
    if (ready) {
        completeUpload(options, base, checkpoint, checkpointPath, cancel, result);
        return;
    }
    uint64_t confirmed = 0;
    std::vector<bool> seen(checkpoint.partsTotal + 1, false);
    for (const Part& part : parts) {
        confirmed += part.size;
        seen[part.number] = true;
    }
    if (progress && progress(confirmed, bytes, progressContext) != 0) {
        result->status = MAI_CLOUD_DRIVE_CANCELED;
        return;
    }
    for (uint32_t number = 1; number <= checkpoint.partsTotal; ++number) {
        if (seen[number]) continue;
        if (canceled(cancel)) {
            result->status = MAI_CLOUD_DRIVE_CANCELED;
            return;
        }
        MaiCloudDriveResult partResult{};
        uploadPart(options, base, source, checkpoint, number, confirmed,
                   cancel, progress, progressContext, &partResult);
        if (partResult.status != MAI_CLOUD_DRIVE_OK) {
            moveResult(result, &partResult);
            return;
        }
        mai_cloud_drive_result_free(&partResult);
        confirmed += expectedPartBytes(checkpoint, number);
    }
    if (!listParts(options, base, checkpoint, cancel, &parts, &ready, result))
        return;
    if (ready) {
        completeUpload(options, base, checkpoint, checkpointPath, cancel, result);
        return;
    }
    if (parts.size() != checkpoint.partsTotal) {
        result->status = MAI_CLOUD_DRIVE_PART_MISMATCH;
        return;
    }
    std::string finalFingerprint;
    result->status = digestSource(source, cancel, &finalFingerprint);
    if (result->status != MAI_CLOUD_DRIVE_OK) return;
    if (finalFingerprint != fingerprint) {
        result->status = MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH;
        return;
    }
    completeUpload(options, base, checkpoint, checkpointPath, cancel, result);
}

extern "C" void mai_cloud_drive_upload_resumable(
    const MaiCloudDriveOptions* options, const char* filePath,
    const char* checkpointPath, const char* displayName,
    const char* contentType, const MaiCloudDriveCancel* cancel,
    MaiCloudDriveProgress progress, void* progressContext,
    MaiCloudDriveResult* result) {
    if (!result) return;
    resetResult(result);
    if (!filePath || !*filePath || std::strlen(filePath) > 4096) return;
    MaiCloudDriveMediaInfo info{};
    if (!displayName || !contentType ||
        !mediaInfo(displayName, &info, options && sessionMode(options)) ||
        std::strcmp(contentType, info.content_type_utf8) != 0) return;
    try {
        if (!std::filesystem::u8path(filePath).is_absolute()) return;
    } catch (...) { return; }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(filePath, "rb"),
                                                     &std::fclose);
    uint64_t bytes = 0;
    if (!file || !fileSize(file.get(), &bytes) ||
        bytes == 0 || bytes > info.max_size_bytes) {
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
        return;
    }
    MaiCloudDriveSource source{filePath, bytes, fileReadAt, file.get()};
    mai_cloud_drive_upload_resumable_source(options, &source, checkpointPath,
        displayName, contentType, cancel, progress, progressContext, result);
}

extern "C" void mai_cloud_drive_stage_stream(
    const char* targetPath, uint64_t expectedBytes,
    MaiCloudDriveReadNext readNext, void* readContext,
    const MaiCloudDriveCancel* cancel, MaiCloudDriveProgress progress,
    void* progressContext, MaiCloudDriveResult* result) {
    if (!result) return;
    resetResult(result);
    if (!targetPath || !*targetPath || std::strlen(targetPath) > 4096 ||
        !readNext || expectedBytes == 0 || expectedBytes > kMaxVideoBytes) return;
    std::filesystem::path target;
    try {
        target = std::filesystem::u8path(targetPath);
        if (!target.is_absolute()) return;
        std::error_code error;
        if (!std::filesystem::is_directory(target.parent_path(), error) || error ||
            std::filesystem::exists(target, error) || error) {
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
            return;
        }
        const auto space = std::filesystem::space(target.parent_path(), error);
        if (error) {
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
            return;
        }
        if (space.available < expectedBytes + 64 * 1024 * 1024) {
            result->status = MAI_CLOUD_DRIVE_INSUFFICIENT_SPACE;
            return;
        }
    } catch (...) {
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
        return;
    }
    const std::string temporary = std::string(targetPath) + ".maicloud-part";
    std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(temporary.c_str(), "wb"),
                                                     &std::fclose);
    if (!file) {
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
        return;
    }
    const auto fail = [&](MaiCloudDriveStatus status) {
        file.reset();
        removeCheckpoint(temporary.c_str());
        result->status = status;
    };
    if (progress && progress(0, expectedBytes, progressContext) != 0) {
        fail(MAI_CLOUD_DRIVE_CANCELED);
        return;
    }
    std::vector<unsigned char> chunk(1024 * 1024);
    uint64_t copied = 0;
    while (copied < expectedBytes) {
        if (canceled(cancel)) {
            fail(MAI_CLOUD_DRIVE_CANCELED);
            return;
        }
        const size_t requested = size_t(std::min<uint64_t>(chunk.size(),
                                                           expectedBytes - copied));
        size_t received = 0;
        if (readNext(readContext, chunk.data(), requested, &received) != 1 ||
            received == 0 || received > requested ||
            std::fwrite(chunk.data(), 1, received, file.get()) != received) {
            fail(MAI_CLOUD_DRIVE_FILE_ERROR);
            return;
        }
        copied += received;
        if (progress && progress(copied, expectedBytes, progressContext) != 0) {
            fail(MAI_CLOUD_DRIVE_CANCELED);
            return;
        }
    }
    size_t extra = 0;
    if (readNext(readContext, chunk.data(), 1, &extra) != 1 || extra != 0 ||
        std::fflush(file.get()) != 0) {
        fail(MAI_CLOUD_DRIVE_FILE_ERROR);
        return;
    }
#ifdef _WIN32
    if (_commit(_fileno(file.get())) != 0) {
#else
    if (fsync(fileno(file.get())) != 0) {
#endif
        fail(MAI_CLOUD_DRIVE_FILE_ERROR);
        return;
    }
    file.reset();
    if (canceled(cancel)) {
        removeCheckpoint(temporary.c_str());
        result->status = MAI_CLOUD_DRIVE_CANCELED;
        return;
    }
    try {
        std::error_code error;
        if (std::filesystem::exists(target, error) || error) {
            removeCheckpoint(temporary.c_str());
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
            return;
        }
    } catch (...) {
        removeCheckpoint(temporary.c_str());
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
        return;
    }
#ifdef _WIN32
    const std::wstring sourceWide = toWide(temporary.c_str());
    const std::wstring targetWide = toWide(targetPath);
    const bool moved = !sourceWide.empty() && !targetWide.empty() &&
        MoveFileExW(sourceWide.c_str(), targetWide.c_str(),
                    MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool moved = std::rename(temporary.c_str(), targetPath) == 0;
#endif
    if (!moved) {
        removeCheckpoint(temporary.c_str());
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
        return;
    }
    result->status = MAI_CLOUD_DRIVE_OK;
}

extern "C" void mai_cloud_drive_list_local_checkpoints(const char* directory,
                                                         MaiCloudDriveResult* result) {
    if (!result) return;
    resetResult(result);
    if (!directory || !*directory || std::strlen(directory) > 4096) return;
    try {
        const std::filesystem::path root = std::filesystem::u8path(directory);
        if (!root.is_absolute()) return;
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) {
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
            return;
        }
        json uploads = json::array();
        size_t invalidCount = 0;
        std::filesystem::directory_iterator iterator(root, error);
        const std::filesystem::directory_iterator end;
        if (error) {
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
            return;
        }
        for (; iterator != end; iterator.increment(error)) {
            if (error || uploads.size() + invalidCount > 1000) break;
            if (iterator->is_symlink(error) ||
                !iterator->is_regular_file(error)) continue;
            if (error) break;
            const std::string path = iterator->path().u8string();
            const std::string suffix = ".maicloud.json";
            if (path.size() < suffix.size() ||
                path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            Checkpoint checkpoint;
            bool exists = false;
            if (!readCheckpoint(path.c_str(), &checkpoint, &exists)) {
                ++invalidCount;
                continue;
            }
            if (exists) uploads.push_back(json{{"checkpoint_path", path},
                {"source_id", checkpoint.sourceId},
                {"video_id", checkpoint.videoId}, {"name", checkpoint.name},
                {"content_type", checkpoint.contentType},
                {"size_bytes", checkpoint.size}});
        }
        if (error) {
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
            return;
        }
        const std::string body = json{{"uploads", uploads},
                                      {"invalid_count", invalidCount}}.dump();
        if (body.size() > kMaxJsonBytes) {
            result->status = MAI_CLOUD_DRIVE_RESPONSE_TOO_LARGE;
            return;
        }
        if (!assignBody(result, body)) {
            result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
            return;
        }
        result->status = MAI_CLOUD_DRIVE_OK;
    } catch (...) {
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
    }
}

extern "C" void mai_cloud_drive_abort_resumable(
    const MaiCloudDriveOptions* options, const char* checkpointPath,
    const MaiCloudDriveCancel* cancel, MaiCloudDriveResult* result) {
    if (!result) return;
    resetResult(result);
    if (!validOptions(options, true) || !checkpointPath || !*checkpointPath ||
        std::strlen(checkpointPath) > 4096) return;
    std::string base;
    if (!validBase(options->auth_base_url_utf8, &base)) return;
    Checkpoint checkpoint;
    bool exists = false;
    if (!readCheckpoint(checkpointPath, &checkpoint, &exists) || !exists) {
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
        return;
    }
    if (checkpoint.roomKey != roomKey(options)) {
        result->status = MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH;
        return;
    }
    requestInternal(options, base,
        apiRoot(options) + "/uploads/" + checkpoint.videoId,
        "DELETE", json::object(), cancel, result);
    if (result->status != MAI_CLOUD_DRIVE_OK) return;
    const json response = parseResponse(*result);
    std::string id;
    std::string status;
    if (!stringField(response, "video_id", &id) || id != checkpoint.videoId ||
        !stringField(response, "status", &status) || status != "deleted") {
        result->status = MAI_CLOUD_DRIVE_INVALID_RESPONSE;
        return;
    }
    if (!removeCheckpoint(checkpointPath))
        result->status = MAI_CLOUD_DRIVE_FILE_ERROR;
}
