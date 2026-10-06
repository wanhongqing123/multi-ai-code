#include "MaiCreativeMediaSupport.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "MaiBlockingCheck.h"
#include "MaiDownloadFileTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;

std::string stringValue(const Json& value, const char* field) {
    return value.is_object() && value.contains(field) && value[field].is_string()
               ? value[field].get<std::string>()
               : std::string{};
}

struct ResponseBuffer {
    std::string bytes;
    bool exceeded = false;
};

std::size_t receive(char* data, std::size_t size, std::size_t count, void* user) {
    auto& response = *static_cast<ResponseBuffer*>(user);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const std::size_t bytes = size * count;
    if (bytes > kMaxResponseBytes - response.bytes.size()) {
        response.exceeded = true;
        return 0;
    }
    response.bytes.append(data, bytes);
    return bytes;
}

int checkCanceled(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const MaiToolContext*>(user)->isCanceled() ? 1 : 0;
}

void configureCurl(CURL* curl, const std::string& url, const std::string& caBundle,
                   ResponseBuffer& response, const MaiToolContext& context, long timeout) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, checkCanceled);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
}

MaiCreativeHttpResult finishResponse(const ResponseBuffer& response, CURLcode transfer,
                                     long httpStatus, const MaiToolContext& context) {
    if (context.isCanceled())
        return {{}, maiCreativeFailure(MaiErrorCode::Canceled, "canceled", "Task was canceled")};
    if (response.exceeded)
        return {{},
                maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                   "Provider response exceeded 2 MB")};
    if (transfer != CURLE_OK)
        return {{},
                maiCreativeFailure(MaiErrorCode::Network, "network", curl_easy_strerror(transfer))};
    const Json data = Json::parse(response.bytes, nullptr, false);
    if (!data.is_object())
        return {{},
                maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                   "Provider returned invalid JSON")};
    const Json base = data.value("base_resp", Json::object());
    const Json providerError = data.value("error", Json::object());
    const int miniMaxStatus =
        base.is_object() && base.value("status_code", Json{}).is_number_integer()
            ? base["status_code"].get<int>()
            : 0;
    const int klingStatus =
        data.value("code", Json{}).is_number_integer() ? data["code"].get<int>() : 0;
    if (httpStatus < 200 || httpStatus >= 300 || miniMaxStatus != 0 || klingStatus != 0) {
        const std::string providerMessage = stringValue(providerError, "message");
        const std::string message = !providerMessage.empty() ? providerMessage
                                    : !stringValue(data, "message").empty()
                                        ? stringValue(data, "message")
                                        : stringValue(base, "status_msg");
        const Json providerCode = !stringValue(providerError, "type").empty()
                                      ? Json(stringValue(providerError, "type"))
                                      : Json(miniMaxStatus != 0 ? miniMaxStatus : klingStatus);
        return {{},
                MaiToolResult::failure(
                    MaiErrorCode::Network,
                    Json{{"code", "provider_error"},
                         {"provider_code", providerCode},
                         {"http_status", httpStatus},
                         {"request_id", stringValue(data, "request_id")},
                         {"message", message.empty() ? "Provider request failed" : message}}
                        .dump())};
    }
    return {response.bytes, std::nullopt};
}

std::string encodeBase64(const std::string& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto a = static_cast<unsigned char>(bytes[index]);
        const auto b = index + 1 < bytes.size() ? static_cast<unsigned char>(bytes[index + 1]) : 0;
        const auto c = index + 2 < bytes.size() ? static_cast<unsigned char>(bytes[index + 2]) : 0;
        encoded.push_back(alphabet[a >> 2]);
        encoded.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        encoded.push_back(index + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        encoded.push_back(index + 2 < bytes.size() ? alphabet[c & 63] : '=');
    }
    return encoded;
}

}  // namespace

MaiToolResult maiCreativeFailure(MaiErrorCode error, const char* code, const std::string& message) {
    return MaiToolResult::failure(error, Json{{"code", code}, {"message", message}}.dump());
}

MaiToolResult maiCreativeInvalid(const std::string& message) {
    return maiCreativeFailure(MaiErrorCode::InvalidInput, "invalid_input", message);
}

bool maiCreativeValidId(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= 'a' && character <= 'z') || character == '-' ||
                      character == '_';
           });
}

MaiCreativeHttpResult maiCreativeRequestJson(const std::string& url, const std::string& key,
                                             const std::string& caBundle, const std::string* body,
                                             const MaiToolContext& context) {
    maiAssertBlockingAllowed("creative_media_request");
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return {
            {},
            maiCreativeFailure(MaiErrorCode::Internal, "internal", "Could not initialize HTTP")};
    ResponseBuffer response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    if (body != nullptr) headers = curl_slist_append(headers, "Content-Type: application/json");
    configureCurl(curl, url, caBundle, response, context, body == nullptr ? 60L : 180L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (body != nullptr) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
    }
    const CURLcode transfer = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return finishResponse(response, transfer, status, context);
}

MaiCreativeHttpResult maiCreativeUploadFile(const std::string& url, const std::string& key,
                                            const std::string& caBundle,
                                            const std::string& filename, const std::string& bytes,
                                            const std::string& purpose,
                                            const MaiToolContext& context) {
    maiAssertBlockingAllowed("creative_media_upload");
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return {
            {},
            maiCreativeFailure(MaiErrorCode::Internal, "internal", "Could not initialize upload")};
    curl_mime* mime = curl_mime_init(curl);
    if (mime == nullptr) {
        curl_easy_cleanup(curl);
        return {{},
                maiCreativeFailure(MaiErrorCode::Internal, "internal",
                                   "Could not create multipart upload")};
    }
    curl_mimepart* purposePart = curl_mime_addpart(mime);
    curl_mimepart* filePart = curl_mime_addpart(mime);
    if (purposePart == nullptr || filePart == nullptr ||
        curl_mime_name(purposePart, "purpose") != CURLE_OK ||
        curl_mime_data(purposePart, purpose.c_str(), purpose.size()) != CURLE_OK ||
        curl_mime_name(filePart, "file") != CURLE_OK ||
        curl_mime_filename(filePart, filename.c_str()) != CURLE_OK ||
        curl_mime_data(filePart, bytes.data(), bytes.size()) != CURLE_OK) {
        curl_mime_free(mime);
        curl_easy_cleanup(curl);
        return {{},
                maiCreativeFailure(MaiErrorCode::Internal, "internal",
                                   "Could not prepare multipart upload")};
    }
    ResponseBuffer response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
    configureCurl(curl, url, caBundle, response, context, 180L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    const CURLcode transfer = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return finishResponse(response, transfer, status, context);
}

std::optional<MaiToolResult> maiCreativeReadImage(const std::string& candidate,
                                                  const MaiToolContext& context, bool dataUrl,
                                                  std::string& encoded) {
    const std::string path = context.resolvePath(candidate);
    if (path.empty()) return maiCreativeInvalid("image_path is not accessible");
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 ||
        size > 5'000'000)
        return maiCreativeInvalid("image_path must contain 1 to 5000000 bytes");
    std::string bytes;
    bool truncated = false;
    if (MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), bytes, 5'000'001, &truncated) ||
        truncated)
        return maiCreativeInvalid("image_path could not be read within 5 MB");
    const bool png = bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xff &&
                      static_cast<unsigned char>(bytes[1]) == 0xd8 &&
                      static_cast<unsigned char>(bytes[2]) == 0xff;
    if (!png && !jpeg) return maiCreativeInvalid("image_path must be PNG or JPEG");
    encoded = (dataUrl ? std::string("data:") + (png ? "image/png" : "image/jpeg") + ";base64,"
                       : std::string{}) +
              encodeBase64(bytes);
    return std::nullopt;
}

MaiToolResult maiCreativeDownloadMedia(const std::string& url, bool video,
                                       const std::string& provider, const MaiToolContext& context,
                                       const std::string& caBundle) {
    if (url.compare(0, 8, "https://") != 0)
        return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                  "Provider returned no HTTPS media URL");
    const std::string relative = provider + "-" + MaiIdGenerator::generate("file_");
    const std::string temporary = relative + "." + MaiIdGenerator::generate("part_") + ".download";
    auto downloader = makeMaiDownloadFileTool(caBundle);
    const MaiToolResult fetched = downloader->execute(Json{{"url", url},
                                                           {"output_path", temporary},
                                                           {"max_size_mb", video ? 500 : 50},
                                                           {"timeout_s", 300}}
                                                          .dump(),
                                                      context);
    if (fetched.hasError()) return fetched;
    const Json downloaded = Json::parse(fetched.output(), nullptr, false);
    const std::string source = stringValue(downloaded, "path");
    std::string prefix;
    if (source.empty() || MaiFileSystem::readFile(MaiFilePath::fromUtf8(source), prefix, 16))
        return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                  "Downloaded media is unreadable");
    const bool mp4 = prefix.size() >= 8 && prefix.compare(4, 4, "ftyp") == 0;
    const bool png = prefix.size() >= 8 && prefix.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = prefix.size() >= 3 && static_cast<unsigned char>(prefix[0]) == 0xff &&
                      static_cast<unsigned char>(prefix[1]) == 0xd8 &&
                      static_cast<unsigned char>(prefix[2]) == 0xff;
    const bool webp = prefix.size() >= 12 && prefix.compare(0, 4, "RIFF") == 0 &&
                      prefix.compare(8, 4, "WEBP") == 0;
    if (video ? !mp4 : !png && !jpeg && !webp) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                  "Provider output is not expected media");
    }
    const std::string extension = video ? ".mp4" : png ? ".png" : jpeg ? ".jpg" : ".webp";
    const std::string target = maiResolvePathWithinRoot(context.root, relative + extension);
    if (target.empty() || MaiFileSystem::exists(MaiFilePath::fromUtf8(target)) ||
        MaiFileSystem::publishNewFile(MaiFilePath::fromUtf8(source),
                                      MaiFilePath::fromUtf8(target))) {
        (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(source));
        return maiCreativeFailure(MaiErrorCode::Internal, "io_error",
                                  "Could not publish provider media");
    }
    return MaiToolResult::success(Json{{"path", target},
                                       {"mime_type", video  ? "video/mp4"
                                                     : png  ? "image/png"
                                                     : jpeg ? "image/jpeg"
                                                            : "image/webp"}}
                                      .dump());
}
