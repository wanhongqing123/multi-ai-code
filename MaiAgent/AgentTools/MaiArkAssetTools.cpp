#include "MaiArkAssetTools.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiCurlCliTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

std::string documentMimeType(const std::string& extension) {
    if (extension == ".docx")
        return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (extension == ".doc") return "application/msword";
    if (extension == ".xlsx")
        return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (extension == ".xls") return "application/vnd.ms-excel";
    if (extension == ".pptx")
        return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    if (extension == ".ppt") return "application/vnd.ms-powerpoint";
    if (extension == ".pdf") return "application/pdf";
    if (extension == ".txt") return "text/plain";
    if (extension == ".key") return "application/vnd.apple.keynote";
    if (extension == ".pages") return "application/vnd.apple.pages";
    if (extension == ".numbers") return "application/vnd.apple.numbers";
    if (extension == ".md") return "text/markdown";
    return {};
}

struct MaiArkHttpResponse {
    long status = 0;
    int attempts = 0;
    std::string body;
    std::string requestId;
    std::string upstreamErrorCode;
};

enum class MaiAssetRetryPolicy { ReadOnly, SignedGrant, IdempotentPut, ConnectOnly };

std::string headerValue(const std::string& raw) {
    const std::size_t colon = raw.find(':');
    if (colon == std::string::npos) return {};
    std::size_t begin = colon + 1;
    while (begin < raw.size() && (raw[begin] == ' ' || raw[begin] == '\t')) ++begin;
    std::size_t end = raw.size();
    while (end > begin && (raw[end - 1] == '\r' || raw[end - 1] == '\n' || raw[end - 1] == ' '))
        --end;
    if (end - begin > 256) return {};
    for (std::size_t index = begin; index < end; ++index)
        if (static_cast<unsigned char>(raw[index]) < 32 ||
            static_cast<unsigned char>(raw[index]) > 126)
            return {};
    return raw.substr(begin, end - begin);
}

std::size_t receiveHttpHeader(char* data, std::size_t size, std::size_t count, void* target) {
    if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) return 0;
    const std::size_t length = size * count;
    if (length > 512) return length;
    auto& response = *static_cast<MaiArkHttpResponse*>(target);
    std::string line(data, length);
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) return length;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (name == "x-request-id" || name == "x-oss-request-id")
        response.requestId = headerValue(line);
    else if (name == "x-error-code")
        response.upstreamErrorCode = headerValue(line);
    return length;
}

bool retryableCurlError(CURLcode result, MaiAssetRetryPolicy policy) {
    if (result == CURLE_COULDNT_RESOLVE_PROXY || result == CURLE_COULDNT_RESOLVE_HOST ||
        result == CURLE_COULDNT_CONNECT || result == CURLE_SSL_CONNECT_ERROR)
        return true;
    if (policy == MaiAssetRetryPolicy::ConnectOnly) return false;
    return result == CURLE_OPERATION_TIMEDOUT || result == CURLE_SEND_ERROR ||
           result == CURLE_RECV_ERROR || result == CURLE_GOT_NOTHING || result == CURLE_HTTP2 ||
           result == CURLE_HTTP2_STREAM;
}

bool retryableHttpStatus(const MaiArkHttpResponse& response, MaiAssetRetryPolicy policy) {
    if (policy == MaiAssetRetryPolicy::ConnectOnly) return false;
    if (response.status != 408 && response.status != 429 && response.status != 500 &&
        response.status != 502 && response.status != 503 && response.status != 504)
        return false;
    if (response.status != 502) return true;
    const Json body = Json::parse(response.body, nullptr, false);
    return !body.is_object() || body.value("provider_code", std::string{}).empty();
}

bool pauseBeforeRetry(int attempt, const MaiToolContext& context) {
    constexpr std::array<int, 4> kDelaySeconds = {2, 5, 10, 20};
    const auto delay = std::chrono::seconds(kDelaySeconds[std::min(attempt, 3)]);
    const auto end = std::chrono::steady_clock::now() + delay;
    while (!context.isCanceled() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return !context.isCanceled();
}

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
                                       const char* stage, const std::string& payload,
                                       const std::vector<std::string>& headerValues,
                                       const std::string& caBundle, const MaiToolContext& context,
                                       MaiAssetRetryPolicy policy) {
    if (!validHttps(url)) return {MaiErrorCode::InvalidInput, "A valid HTTPS URL is required"};
    maiAssertBlockingAllowed("ark_asset_tool");
    // 查询和签发链接可以对短暂的网络/服务故障做有界重试；创建资产这类写操作
    // 只在请求尚未发出去的连接错误下重试，避免重复创建。
    const int maximumAttempts = policy == MaiAssetRetryPolicy::ConnectOnly     ? 3
                                : policy == MaiAssetRetryPolicy::IdempotentPut ? 2
                                                                               : 5;
    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
        if (context.isCanceled()) return {MaiErrorCode::Canceled, "Asset request was canceled"};
        CURL* curl = curl_easy_init();
        if (curl == nullptr) return {MaiErrorCode::Internal, "Could not initialize HTTP"};
        std::array<char, CURL_ERROR_SIZE> transportError{};
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, transportError.data());
        struct curl_slist* headers = nullptr;
        for (const std::string& value : headerValues)
            headers = curl_slist_append(headers, value.c_str());
        MaiArkHttpResponse response;
        response.attempts = attempt + 1;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveHttpBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receiveHttpHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(
            curl, CURLOPT_TIMEOUT,
            policy == MaiAssetRetryPolicy::ReadOnly || policy == MaiAssetRetryPolicy::SignedGrant
                ? 15L
                : 180L);
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
        char* primaryIp = nullptr;
        curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primaryIp);
        const std::string resolvedIp = primaryIp != nullptr ? primaryIp : "";
        const std::string detail = transportError.data();
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (context.isCanceled()) return {MaiErrorCode::Canceled, "Asset request was canceled"};
        if (result == CURLE_OK) {
            if (attempt + 1 < maximumAttempts && retryableHttpStatus(response, policy)) {
                if (!pauseBeforeRetry(attempt, context))
                    return {MaiErrorCode::Canceled, "Asset request was canceled"};
                continue;
            }
            return response;
        }
        if (attempt + 1 < maximumAttempts && retryableCurlError(result, policy)) {
            if (!pauseBeforeRetry(attempt, context))
                return {MaiErrorCode::Canceled, "Asset request was canceled"};
            continue;
        }
        // 把阶段、HTTP 状态、供应商错误码和 request ID 交回主模型，便于区分
        // “没发出去”“服务暂时不可用”和“上游明确拒绝”。
        // 错误缓冲区只保留短的非 URL 信息，避免签名 OSS 链接或令牌进入工具结果。
        const bool safeDetail = detail.size() <= 256 && detail.find("://") == std::string::npos &&
                                detail.find('?') == std::string::npos &&
                                detail.find("Bearer") == std::string::npos &&
                                detail.find("Authorization") == std::string::npos;
        return {MaiErrorCode::Network, Json{{"code", "asset_http_failed"},
                                            {"stage", stage},
                                            {"curl_code", static_cast<int>(result)},
                                            {"resolved_ip", resolvedIp},
                                            {"transport_detail", safeDetail ? detail : ""},
                                            {"http_status", response.status},
                                            {"http_response_received", response.status != 0},
                                            {"request_id", response.requestId},
                                            {"upstream_error_code", response.upstreamErrorCode},
                                            {"attempts", response.attempts},
                                            {"message", curl_easy_strerror(result)}}
                                           .dump()};
    }
    return {MaiErrorCode::Internal, "Asset request retry state is invalid"};
}

MaiResult<std::string> uploadImageToOss(const std::string& candidate,
                                        const MaiArkAssetServiceSettings& settings,
                                        const std::string& caBundle,
                                        const MaiToolContext& context) {
    const std::string path = context.resolvePath(candidate);
    const MaiFilePath source = MaiFilePath::fromUtf8(path);
    std::uint64_t fileSize = 0;
    if (path.empty() || MaiFileSystem::isSymbolicLink(source) ||
        !MaiFileSystem::fileSize(source, fileSize) || fileSize == 0 || fileSize > 30'000'000)
        return {MaiErrorCode::InvalidInput, "Image file is unavailable or too large"};
    std::string fileName = source.baseName().toUtf8();
    std::transform(fileName.begin(), fileName.end(), fileName.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const bool png = fileName.size() >= 4 && fileName.substr(fileName.size() - 4) == ".png";
    const bool jpeg = (fileName.size() >= 4 && fileName.substr(fileName.size() - 4) == ".jpg") ||
                      (fileName.size() >= 5 && fileName.substr(fileName.size() - 5) == ".jpeg");
    if (!png && !jpeg) return {MaiErrorCode::InvalidInput, "OSS accepts JPG or PNG images"};
    const std::string authorization = "Authorization: Bearer " + settings.token;
    const Json signBody = {{"filename", fileName}, {"size_bytes", fileSize}};
    auto signedResponse =
        sendHttp(serviceEndpoint(settings.baseUrl, "sign-upload"), "POST", "sign_upload",
                 signBody.dump(), {authorization, "Content-Type: application/json"}, caBundle,
                 context, MaiAssetRetryPolicy::SignedGrant);
    if (!signedResponse) return signedResponse.error();
    if (signedResponse.value().status != 200) {
        const Json detail = Json::parse(signedResponse.value().body, nullptr, false);
        return {MaiErrorCode::Network,
                Json{{"code", "upload_signing_failed"},
                     {"stage", "sign_upload"},
                     {"http_status", signedResponse.value().status},
                     {"request_id", signedResponse.value().requestId},
                     {"attempts", signedResponse.value().attempts},
                     {"message", detail.is_object()
                                     ? detail.value("error", std::string("OSS signing failed"))
                                     : std::string("OSS signing failed")}}
                    .dump()};
    }
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
    const std::string expectedType = png ? "image/png" : "image/jpeg";
    if (!validHttps(uploadUrl) || !validHttps(readUrl) || contentType != expectedType ||
        uploadUrl.substr(0, uploadUrl.find('?')) != readUrl.substr(0, readUrl.find('?')))
        return {MaiErrorCode::Protocol, "OSS signer returned invalid image links"};
    std::string bytes;
    bool truncated = false;
    if (MaiError error = MaiFileSystem::readFile(source, bytes, 30'000'000, &truncated))
        return error;
    if (truncated || bytes.size() != fileSize)
        return {MaiErrorCode::InvalidInput, "Image changed during upload"};
    auto uploaded =
        sendHttp(uploadUrl, "PUT", "oss_upload", bytes, {"Content-Type: " + contentType, "Expect:"},
                 caBundle, context, MaiAssetRetryPolicy::IdempotentPut);
    if (!uploaded) return uploaded.error();
    if (uploaded.value().status < 200 || uploaded.value().status >= 300)
        return {MaiErrorCode::Network, Json{{"code", "oss_upload_failed"},
                                            {"stage", "oss_upload"},
                                            {"http_status", uploaded.value().status},
                                            {"request_id", uploaded.value().requestId},
                                            {"attempts", uploaded.value().attempts},
                                            {"message", "OSS rejected the image upload"}}
                                           .dump()};
    return readUrl;
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
        // 本地图入库有三步：服务器签发 OSS 链接、客户端直传原图、再调用 Ark CreateAsset。
        // 传给 Ark 的是临时 HTTPS 读取地址，不是手机上的文件路径。
        const auto uploaded = uploadImageToOss(request.value("image_path", std::string{}), settings,
                                               caBundle, context);
        if (!uploaded) return uploaded.error();
        request.erase("image_path");
        request["action"] = "create_asset";
        request["url"] = uploaded.value();
    }
    const std::string action = request["action"].get<std::string>();
    // 只读动作与写入动作使用不同的重试策略；写入结果未知时不能盲目重放。
    const MaiAssetRetryPolicy policy =
        action == "list_groups" || action == "list_assets" || action == "get_asset"
            ? MaiAssetRetryPolicy::ReadOnly
            : MaiAssetRetryPolicy::ConnectOnly;
    auto result = sendHttp(serviceEndpoint(settings.baseUrl, "ark-assets"), "POST", "ark_assets",
                           request.dump(), {authorization, "Content-Type: application/json"},
                           caBundle, context, policy);
    if (!result) return result.error();
    const Json envelope = Json::parse(result.value().body, nullptr, false);
    if (!envelope.is_object())
        return {MaiErrorCode::Protocol, "Ark Assets service returned invalid JSON"};
    if (result.value().status != 200)
        return {
            MaiErrorCode::Network,
            Json{{"code", "provider_error"},
                 {"stage", "ark_assets"},
                 {"http_status", result.value().status},
                 {"provider_code",
                  envelope.value("provider_code", result.value().upstreamErrorCode)},
                 {"request_id", envelope.value("request_id", result.value().requestId)},
                 {"attempts", result.value().attempts},
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

void removeAssetSignedUrls(Json& value) {
    if (value.is_array()) {
        for (Json& item : value) removeAssetSignedUrls(item);
        return;
    }
    if (!value.is_object()) return;
    // Ark 返回的 URL 可能包含短期签名；资产 ID 足够用于后续生成，无需回灌给模型。
    value.erase("URL");
    for (auto& entry : value.items()) removeAssetSignedUrls(entry.value());
}

class MaiArkAssetTool final : public MaiTool {
public:
    explicit MaiArkAssetTool(MaiArkAssetProvider provider) : mProvider(std::move(provider)) {}

    std::string name() const override {
        return "ark_assets";
    }

    std::string description() const override {
        // 英文描述按以下顺序向主模型说明完整资产流程：
        // 1. list_groups/create_group 管理组；list_assets/get_asset 查询素材状态。
        //    同一人物或同一批相关照片优先复用合适组，不为每张图都新建组。
        // 2. create_asset 接受方舟可读取的 HTTPS 图像 URL；upload_image 接受本地图，
        //    先经短期 OSS 签名直传原图，再把读取 URL 登记到 Ark。图片上传与入库
        //    是异步的，必须用 get_asset 等到 Active 或 Failed。
        // 3. 普通 AIGC 素材登记不等于真人肖像授权。真人验证由
        //    begin_real_validation 返回一次性 H5 链接，本人完成后用
        //    get_real_validation 取对应组 ID。凭证仅 30 分钟有效。
        // 4. 同一个真人验证组只属于一个人，多人同框可能被拒。只有 Active 的
        //    素材 ID 能以 asset://<ID> 交给 Seedance；项目必须与视频 Key 匹配。
        // 5. 平台仍可能拒绝图片；工具返回真实状态和原因，不暗示入库必定过审。
        //    AK/SK 留在服务端，模型也不应看到服务令牌或签名 URL。
        return "Manage Seedance private avatar assets. Use list_groups, create_group, "
               "create_asset, upload_image, get_asset, list_assets, begin_real_validation, "
               "or get_real_validation. For a real portrait, begin_real_validation returns "
               "a one-time H5 link for that person; after they finish, get_real_validation "
               "returns the verified group ID. Each verified group belongs to one person; "
               "multi-face images may be rejected. The token expires after 30 minutes. "
               "Reuse a suitable group "
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
        // action 决定读/写动作；name/description 只用于新建或命名素材。
        // group_id 关联素材组，group_type 区分 AIGC 与真人验证组；
        // url 仅供 create_asset，image_path 仅供 upload_image 读取本地文件；
        // asset_id 用于 get_asset，byted_token 用于 H5 结果查询；
        // next_token 用于列表分页。下面仅是形状约束，execute 再按动作核对必需字段。
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","list_groups","create_group","create_asset","upload_image","get_asset","list_assets","begin_real_validation","get_real_validation"]},"name":{"type":"string"},"description":{"type":"string"},"group_id":{"type":"string"},"group_type":{"type":"string","enum":["AIGC","LivenessFace"]},"url":{"type":"string"},"image_path":{"type":"string"},"asset_id":{"type":"string"},"byted_token":{"type":"string"},"next_token":{"type":"string"}},"required":["action"],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        const std::string action = field(args, "action");
        return action != "discover" && action != "list_groups" && action != "list_assets" &&
               action != "get_asset" && action != "get_real_validation";
    }

    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        return field(args, "action") == "begin_real_validation";
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
                       "list_assets", "begin_real_validation", "get_real_validation"}},
                     {"asset_type", "AIGC"},
                     {"asset_types", {"AIGC", "LivenessFace"}},
                     {"usage", "Use asset://<ID> only after get_asset reports Active"}}
                    .dump());
        if (!mProvider)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "Ark Assets service is not configured on this device");
        Json request = {{"action", action}};
        // 先在工具层拒绝缺字段或越界路径；通过后才请求服务器，避免无效远端调用。
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
            const std::string groupType = field(args, "group_type");
            if (!groupType.empty() && groupType != "AIGC" && groupType != "LivenessFace")
                return invalid("group_type must be AIGC or LivenessFace");
            if (!groupType.empty()) request["group_type"] = groupType;
        } else if (action == "begin_real_validation") {
            // 由服务端固定 H5 回调地址，调用者不能指定跳转目标。
        } else if (action == "get_real_validation") {
            const std::string token = field(args, "byted_token");
            if (token.empty() || token.size() > 512 ||
                !std::all_of(token.begin(), token.end(),
                             [](unsigned char ch) { return ch >= 33 && ch <= 126; }))
                return invalid("byted_token from begin_real_validation is required");
            request["byted_token"] = token;
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
        if (action == "begin_real_validation" && (!result.value("BytedToken", Json{}).is_string() ||
                                                  !result.value("H5Link", Json{}).is_string() ||
                                                  !validHttps(result["H5Link"].get<std::string>())))
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          "Ark did not return a valid H5 verification link");
        // 仅将资产状态、ID 等可复用信息回灌模型；带临时签名的 URL 不能展示。
        Json output = result;
        removeAssetSignedUrls(output);
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

static MaiCreativeMediaUploadProvider makePrivateOssStreamUploader(
    std::function<MaiResult<MaiArkAssetServiceSettings>()> settings, std::string caBundlePath,
    bool allowDocuments) {
    return [settings = std::move(settings), caBundlePath = std::move(caBundlePath), allowDocuments](
               const std::string& path, const MaiToolContext& context) -> MaiResult<std::string> {
        const MaiResult<MaiArkAssetServiceSettings> configuration = settings();
        if (!configuration) return configuration.error();
        if (!validHttps(configuration.value().baseUrl) || configuration.value().token.size() < 32)
            return {MaiErrorCode::NotConfigured, "OSS signing service is not configured"};
        const std::string resolved = context.resolvePath(path);
        const MaiFilePath source = MaiFilePath::fromUtf8(resolved);
        std::uint64_t fileSize = 0;
        if (resolved.empty() || MaiFileSystem::isSymbolicLink(source) ||
            !MaiFileSystem::fileSize(source, fileSize) || fileSize == 0)
            return {MaiErrorCode::InvalidInput, "Media file is unavailable"};
        std::string fileName = source.baseName().toUtf8();
        const std::size_t dot = fileName.find_last_of('.');
        std::string extension = dot == std::string::npos ? std::string{} : fileName.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const bool video = extension == ".mp4" || extension == ".mov";
        const std::string documentType = allowDocuments ? documentMimeType(extension) : "";
        const bool document = !documentType.empty();
        const std::uint64_t maximumBytes = video ? 200'000'000 : 100'000'000;
        if ((!video && !document) || fileSize > maximumBytes)
            return {MaiErrorCode::InvalidInput,
                    allowDocuments ? "Use MP4/MOV under 200 MB or a supported document under 100 MB"
                                   : "video_path must be MP4 or MOV under 200 MB"};

        // 服务端只签发短期链接，不接收视频字节；每次上传都使用新的对象名。
        const std::string authorization = "Authorization: Bearer " + configuration.value().token;
        const Json signBody = {{"filename", fileName}, {"size_bytes", fileSize}};
        const auto signedResponse = sendHttp(
            serviceEndpoint(configuration.value().baseUrl, "sign-upload"), "POST",
            "media_sign_upload", signBody.dump(), {authorization, "Content-Type: application/json"},
            caBundlePath, context, MaiAssetRetryPolicy::SignedGrant);
        if (!signedResponse) return signedResponse.error();
        if (signedResponse.value().status != 200)
            return {MaiErrorCode::Network, Json{{"code", "media_signing_failed"},
                                                {"http_status", signedResponse.value().status},
                                                {"request_id", signedResponse.value().requestId}}
                                               .dump()};
        const Json signedLinks = Json::parse(signedResponse.value().body, nullptr, false);
        if (!signedLinks.is_object() || !signedLinks.value("upload_url", Json{}).is_string() ||
            !signedLinks.value("read_url", Json{}).is_string() ||
            !signedLinks.value("upload_headers", Json{}).is_object() ||
            !signedLinks["upload_headers"].value("Content-Type", Json{}).is_string())
            return {MaiErrorCode::Protocol, "OSS signer returned invalid media links"};
        const std::string uploadUrl = signedLinks["upload_url"].get<std::string>();
        const std::string readUrl = signedLinks["read_url"].get<std::string>();
        const std::string contentType =
            signedLinks["upload_headers"]["Content-Type"].get<std::string>();
        const std::string expectedType = document              ? documentType
                                         : extension == ".mov" ? "video/quicktime"
                                                               : "video/mp4";
        if (!validHttps(uploadUrl) || !validHttps(readUrl) || contentType != expectedType ||
            uploadUrl.substr(0, uploadUrl.find('?')) != readUrl.substr(0, readUrl.find('?')))
            return {MaiErrorCode::Protocol, "OSS signer returned mismatched media links"};

        // 内嵌 curl 从文件流式 PUT 到 OSS，避免把最多 200 MB 的视频整段读入内存。
        auto curlTool = makeMaiCurlCliTool(caBundlePath);
        const Json curlRequest = {{"arguments",
                                   {"--request", "PUT", "--header", "Content-Type: " + contentType,
                                    "--header", "Expect:", "--upload-file", resolved, uploadUrl}},
                                  {"timeout_s", 600},
                                  {"max_size_mb", video ? 200 : 100}};
        const MaiToolResult uploaded = curlTool->execute(curlRequest.dump(), context);
        if (uploaded.hasError()) {
            if (context.isCanceled()) return {MaiErrorCode::Canceled, "Media upload was canceled"};
            const Json detail = Json::parse(uploaded.error().message(), nullptr, false);
            return {uploaded.error().code(),
                    Json{{"code", "oss_media_upload_failed"},
                         {"exit_status", detail.is_object() ? detail.value("exit_status", 0) : 0},
                         {"message", "Direct OSS media upload failed"}}
                        .dump()};
        }
        const Json result = Json::parse(uploaded.output(), nullptr, false);
        const int status = result.is_object() ? result.value("status", 0) : 0;
        if (status < 200 || status >= 300)
            return {MaiErrorCode::Network, Json{{"code", "oss_media_upload_failed"},
                                                {"http_status", status},
                                                {"message", "OSS rejected the media upload"}}
                                               .dump()};
        return readUrl;
    };
}

std::function<MaiResult<std::string>(const std::string&, const MaiToolContext&)>
makeMaiPrivateOssVideoUploader(std::function<MaiResult<MaiArkAssetServiceSettings>()> settings,
                               std::string caBundlePath) {
    return makePrivateOssStreamUploader(std::move(settings), std::move(caBundlePath), false);
}

MaiCreativeMediaUploadProvider makeMaiPrivateOssMediaUploader(
    std::function<MaiResult<MaiArkAssetServiceSettings>()> settings, std::string caBundlePath) {
    auto uploadStream = makePrivateOssStreamUploader(settings, caBundlePath, true);
    return [settings = std::move(settings), caBundlePath = std::move(caBundlePath),
            uploadStream = std::move(uploadStream)](
               const std::string& path, const MaiToolContext& context) -> MaiResult<std::string> {
        std::string name = MaiFilePath::fromUtf8(path).baseName().toUtf8();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const std::size_t dot = name.find_last_of('.');
        const std::string extension = dot == std::string::npos ? "" : name.substr(dot);
        if (extension == ".mp4" || extension == ".mov" || !documentMimeType(extension).empty())
            return uploadStream(path, context);
        if (extension != ".jpg" && extension != ".jpeg" && extension != ".png")
            return {MaiErrorCode::InvalidInput, "OSS accepts JPG/PNG images or MP4/MOV videos"};
        const MaiResult<MaiArkAssetServiceSettings> configuration = settings();
        if (!configuration) return configuration.error();
        if (!validHttps(configuration.value().baseUrl) || configuration.value().token.size() < 32)
            return {MaiErrorCode::NotConfigured, "OSS signing service is not configured"};
        return uploadImageToOss(path, configuration.value(), caBundlePath, context);
    };
}
