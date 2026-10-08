#include "MaiArkAssetTools.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

struct MaiArkHttpResponse {
    long status = 0;
    std::string body;
};

std::size_t receiveHttpBody(char* data, std::size_t size, std::size_t count, void* target) {
    if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) return 0;
    const std::size_t length = size * count;
    auto& response = *static_cast<std::string*>(target);
    if (response.size() > 1'000'000 || length > 1'000'000 - response.size()) return 0;
    response.append(data, length);
    return length;
}

int cancelHttp(void* target, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const MaiToolContext*>(target)->isCanceled() ? 1 : 0;
}

bool validHttps(const std::string& text) {
    if (text.rfind("https://", 0) != 0 || text.size() > 4096) return false;
    CURLU* url = curl_url();
    if (url == nullptr) return false;
    const bool parsed = curl_url_set(url, CURLUPART_URL, text.c_str(), 0) == CURLUE_OK;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    char* fragment = nullptr;
    const bool hasHost = parsed && curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                         host != nullptr && *host != '\0';
    const bool hasUser = parsed && curl_url_get(url, CURLUPART_USER, &user, 0) == CURLUE_OK;
    const bool hasPassword =
        parsed && curl_url_get(url, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK;
    const bool hasFragment =
        parsed && curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_OK;
    if (host != nullptr) curl_free(host);
    if (user != nullptr) curl_free(user);
    if (password != nullptr) curl_free(password);
    if (fragment != nullptr) curl_free(fragment);
    curl_url_cleanup(url);
    return hasHost && !hasUser && !hasPassword && !hasFragment;
}

std::string serviceEndpoint(std::string base, const char* action) {
    const std::string oldSuffix = "/sign-upload";
    if (base.size() >= oldSuffix.size() &&
        base.compare(base.size() - oldSuffix.size(), oldSuffix.size(), oldSuffix) == 0)
        base.resize(base.size() - oldSuffix.size());
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/" + action;
}

MaiResult<MaiArkHttpResponse> sendHttp(const std::string& url, const char* method,
                                       const std::string& payload,
                                       const std::vector<std::string>& headerValues,
                                       const std::string& caBundle, const MaiToolContext& context) {
    if (!validHttps(url)) return {MaiErrorCode::InvalidInput, "A valid HTTPS URL is required"};
    maiAssertBlockingAllowed("ark_asset_tool");
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return {MaiErrorCode::Internal, "Could not initialize HTTP"};
    struct curl_slist* headers = nullptr;
    for (const std::string& value : headerValues)
        headers = curl_slist_append(headers, value.c_str());
    MaiArkHttpResponse response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveHttpBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelHttp);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (context.isCanceled()) return {MaiErrorCode::Canceled, "Asset request was canceled"};
    if (result != CURLE_OK) return {MaiErrorCode::Network, "Asset HTTP request failed"};
    return response;
}

MaiResult<std::string> invokeAssetService(const std::string& raw,
                                          const MaiArkAssetServiceSettings& settings,
                                          const std::string& caBundle,
                                          const MaiToolContext& context) {
    if (!validHttps(settings.baseUrl) || settings.token.size() < 32)
        return {MaiErrorCode::NotConfigured, "Ark Assets service is not configured"};
    Json request = Json::parse(raw, nullptr, false);
    if (!request.is_object() || !request.value("action", Json{}).is_string())
        return {MaiErrorCode::InvalidInput, "Invalid asset request"};
    const std::string authorization = "Authorization: Bearer " + settings.token;
    if (request["action"] == "upload_image") {
        const std::string path = request.value("image_path", std::string{});
        const MaiFilePath source = MaiFilePath::fromUtf8(path);
        std::uint64_t fileSize = 0;
        if (path.empty() || MaiFileSystem::isSymbolicLink(source) ||
            !MaiFileSystem::fileSize(source, fileSize) || fileSize == 0 || fileSize > 30'000'000)
            return {MaiErrorCode::InvalidInput, "Image file is unavailable or too large"};
        const Json signBody = {{"filename", source.baseName().toUtf8()}, {"size_bytes", fileSize}};
        auto signedResponse =
            sendHttp(serviceEndpoint(settings.baseUrl, "sign-upload"), "POST", signBody.dump(),
                     {authorization, "Content-Type: application/json"}, caBundle, context);
        if (!signedResponse) return signedResponse.error();
        if (signedResponse.value().status != 200)
            return {MaiErrorCode::Network, Json{{"code", "upload_not_configured"},
                                                {"message", "OSS upload signing failed"}}
                                               .dump()};
        const Json signedLinks = Json::parse(signedResponse.value().body, nullptr, false);
        if (!signedLinks.is_object() || !signedLinks.value("upload_url", Json{}).is_string() ||
            !signedLinks.value("read_url", Json{}).is_string() ||
            !signedLinks.value("upload_headers", Json{}).is_object() ||
            !signedLinks["upload_headers"].value("Content-Type", Json{}).is_string())
            return {MaiErrorCode::Protocol, "OSS signer returned invalid links"};
        const std::string uploadUrl = signedLinks["upload_url"].get<std::string>();
        const std::string readUrl = signedLinks["read_url"].get<std::string>();
        const std::string contentType =
            signedLinks["upload_headers"]["Content-Type"].get<std::string>();
        std::string fileName = source.baseName().toUtf8();
        std::transform(fileName.begin(), fileName.end(), fileName.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const bool png = fileName.size() >= 4 && fileName.substr(fileName.size() - 4) == ".png";
        const std::string expectedType = png ? "image/png" : "image/jpeg";
        if (!validHttps(uploadUrl) || !validHttps(readUrl) || contentType != expectedType)
            return {MaiErrorCode::Protocol, "OSS signer returned invalid image links"};
        std::string bytes;
        bool truncated = false;
        if (MaiError error = MaiFileSystem::readFile(source, bytes, 30'000'000, &truncated))
            return error;
        if (truncated || bytes.size() != fileSize)
            return {MaiErrorCode::InvalidInput, "Image changed during upload"};
        auto uploaded = sendHttp(uploadUrl, "PUT", bytes,
                                 {"Content-Type: " + contentType, "Expect:"}, caBundle, context);
        if (!uploaded) return uploaded.error();
        if (uploaded.value().status < 200 || uploaded.value().status >= 300)
            return {MaiErrorCode::Network, "OSS rejected the image upload"};
        request.erase("image_path");
        request["action"] = "create_asset";
        request["url"] = readUrl;
    }
    auto result = sendHttp(serviceEndpoint(settings.baseUrl, "ark-assets"), "POST", request.dump(),
                           {authorization, "Content-Type: application/json"}, caBundle, context);
    if (!result) return result.error();
    const Json envelope = Json::parse(result.value().body, nullptr, false);
    if (!envelope.is_object())
        return {MaiErrorCode::Protocol, "Ark Assets service returned invalid JSON"};
    if (result.value().status != 200)
        return {
            MaiErrorCode::Network,
            Json{{"code", "provider_error"},
                 {"provider_code", envelope.value("provider_code", std::string{})},
                 {"message", envelope.value("provider_message", std::string{}).empty()
                                 ? envelope.value("error", std::string("Ark Assets request failed"))
                                 : envelope.value("provider_message", std::string{})}}
                .dump()};
    if (!envelope.value("result", Json{}).is_object())
        return {MaiErrorCode::Protocol, "Ark Assets service returned no result"};
    return envelope["result"].dump();
}

std::string field(const Json& object, const char* key) {
    if (!object.contains(key) || !object[key].is_string()) return {};
    return object[key].get<std::string>();
}

bool validId(const std::string& id, const char* prefix) {
    return id.rfind(prefix, 0) == 0 && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_';
           });
}

MaiToolResult invalid(const char* message) {
    return MaiToolResult::failure(MaiErrorCode::InvalidInput, message);
}

class MaiArkAssetTool final : public MaiTool {
public:
    explicit MaiArkAssetTool(MaiArkAssetProvider provider) : mProvider(std::move(provider)) {}

    std::string name() const override {
        return "ark_assets";
    }

    std::string description() const override {
        return "Manage Seedance private virtual avatar assets. Use list_groups, create_group, "
               "create_asset, upload_image, get_asset, or list_assets. Reuse a suitable group "
               "for related photos; do not create a new group for every image. create_asset "
               "submits an existing Ark-accessible HTTPS image URL directly. For local JPEG/PNG "
               "files, "
               "upload_image sends the original source directly to private OSS through a "
               "short-lived signed upload URL, then registers its signed read URL. Ark "
               "processes uploads asynchronously: call get_asset until Status is Active or Failed. "
               "Only an Active asset ID can be passed to seedance_video as "
               "virtual_avatar_asset_id. Ark may reject a submission after upload; return its "
               "actual status and reason. The asset project must match the video API key project. "
               "The service keeps Ark Assets AK/SK; local-file upload also requires its OSS "
               "signer. Do not alter a real face merely to avoid provider review.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","list_groups","create_group","create_asset","upload_image","get_asset","list_assets"]},"name":{"type":"string"},"description":{"type":"string"},"group_id":{"type":"string"},"url":{"type":"string"},"image_path":{"type":"string"},"asset_id":{"type":"string"},"next_token":{"type":"string"}},"required":["action"],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        const std::string action = field(args, "action");
        return action != "discover" && action != "list_groups" && action != "list_assets" &&
               action != "get_asset";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return invalid("action is required");
        const std::string action = field(args, "action");
        if (action == "discover")
            return MaiToolResult::success(
                Json{{"configured", static_cast<bool>(mProvider)},
                     {"actions",
                      {"list_groups", "create_group", "create_asset", "upload_image", "get_asset",
                       "list_assets"}},
                     {"asset_type", "AIGC"},
                     {"usage", "Use asset://<ID> only after get_asset reports Active"}}
                    .dump());
        if (!mProvider)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "Ark Assets service is not configured on this device");
        Json request = {{"action", action}};
        if (action == "create_group") {
            const std::string name = field(args, "name");
            if (name.empty() || name.size() > 64)
                return invalid("name is required (up to 64 bytes)");
            request["name"] = name;
            request["description"] = field(args, "description");
        } else if (action == "create_asset") {
            const std::string groupId = field(args, "group_id");
            const std::string url = field(args, "url");
            if (!validId(groupId, "group-")) return invalid("valid group_id is required");
            if (url.rfind("https://", 0) != 0 || url.size() > 3000)
                return invalid("url must be an Ark-accessible HTTPS image URL");
            request["group_id"] = groupId;
            request["url"] = url;
            request["name"] = field(args, "name");
        } else if (action == "upload_image") {
            const std::string groupId = field(args, "group_id");
            const std::string source = context.resolvePath(field(args, "image_path"));
            if (!validId(groupId, "group-")) return invalid("valid group_id is required");
            const MaiFilePath sourcePath = MaiFilePath::fromUtf8(source);
            std::uint64_t size = 0;
            if (source.empty() || MaiFileSystem::isSymbolicLink(sourcePath) ||
                !MaiFileSystem::fileSize(sourcePath, size) || size == 0 || size > 30'000'000)
                return invalid("image_path must be an accessible regular file");
            const std::string base = sourcePath.baseName().toUtf8();
            const std::size_t dot = base.find_last_of('.');
            std::string extension = dot == std::string::npos ? std::string{} : base.substr(dot);
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (extension != ".jpg" && extension != ".jpeg" && extension != ".png")
                return invalid("image_path must be JPEG or PNG");
            request["group_id"] = groupId;
            request["image_path"] = source;
            request["name"] = field(args, "name");
        } else if (action == "get_asset") {
            const std::string assetId = field(args, "asset_id");
            if (!validId(assetId, "asset-")) return invalid("valid asset_id is required");
            request["asset_id"] = assetId;
        } else if (action == "list_assets" || action == "list_groups") {
            const std::string groupId = field(args, "group_id");
            if (!groupId.empty() && !validId(groupId, "group-"))
                return invalid("group_id is invalid");
            if (!groupId.empty()) request["group_id"] = groupId;
            const std::string nextToken = field(args, "next_token");
            if (nextToken.size() > 2048) return invalid("next_token is too long");
            if (!nextToken.empty()) request["next_token"] = nextToken;
        } else {
            return invalid("unknown Ark Assets action");
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "Ark Assets request was canceled");
        const MaiResult<std::string> response = mProvider(request.dump(), context);
        if (!response)
            return MaiToolResult::failure(response.error().code(), response.error().message());
        const Json result = Json::parse(response.value(), nullptr, false);
        if (!result.is_object())
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          "Ark Assets returned invalid JSON");
        Json output = result;
        if (action == "get_asset" && result.value("Status", Json{}) == "Active" &&
            result.value("Id", Json{}).is_string())
            output["asset_uri"] = "asset://" + result["Id"].get<std::string>();
        return MaiToolResult::success(output.dump());
    }

private:
    MaiArkAssetProvider mProvider;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiArkAssetTool(MaiArkAssetProvider provider) {
    return std::make_unique<MaiArkAssetTool>(std::move(provider));
}

MaiArkAssetProvider makeMaiArkAssetServiceProvider(
    std::function<MaiResult<MaiArkAssetServiceSettings>()> settings, std::string caBundlePath) {
    return
        [settings = std::move(settings), caBundlePath = std::move(caBundlePath)](
            const std::string& request, const MaiToolContext& context) -> MaiResult<std::string> {
            const MaiResult<MaiArkAssetServiceSettings> configuration = settings();
            if (!configuration) return configuration.error();
            return invokeAssetService(request, configuration.value(), caBundlePath, context);
        };
}
