// 文件作用：封装火山方舟媒体任务的提交、查询和结果取回。
// 直接依赖：MaiBlockingCheck.h、MaiDownloadFileTool.h、MaiFilePath.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiArkMediaTools.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiDownloadFileTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

bool isPaidGenerationAction(const std::string& raw) {
    // cancel/delete 不新建付费任务，在普通模式仍逐次确认。
    const Json args = Json::parse(raw, nullptr, false);
    if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
    const std::string action = args["action"].get<std::string>();
    return action != "discover" && action != "prepare" && action != "continue";
}

bool isBillableSubmissionAction(const std::string& raw) {
    const Json args = Json::parse(raw, nullptr, false);
    if (!args.is_object() || !args.value("action", Json{}).is_string()) return false;
    const std::string action = args["action"].get<std::string>();
    return action == "delegate" || action == "revise";
}

std::string arkAssetId(const std::string& value) {
    const std::string id = value.rfind("asset://", 0) == 0 ? value.substr(8) : value;
    if (id.size() < 10 || id.size() > 128 || id.rfind("asset-", 0) != 0 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_';
        }))
        return {};
    return id;
}

bool validArkGroupId(const std::string& id) {
    if (id.size() < 7 || id.size() > 128 || id.rfind("group-", 0) != 0) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               ch == '-' || ch == '_';
    });
}

constexpr char kVideoModel[] = "doubao-seedance-2-5-260628";
constexpr char kStandardVideoModel[] = "doubao-seedance-2-0-260128";
constexpr char kFastVideoModel[] = "doubao-seedance-2-0-fast-260128";
constexpr char kMiniVideoModel[] = "doubao-seedance-2-0-mini-260615";
constexpr const char* kDefaultVideoModel = kMiniVideoModel;
constexpr char kImageModel[] = "doubao-seedream-5-0-flash-260915";
constexpr char kVideoTasksUrl[] =
    "https://ark.cn-beijing.volces.com/api/v3/contents/generations/tasks";
constexpr char kImagesUrl[] = "https://ark.cn-beijing.volces.com/api/v3/images/generations";
constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;

struct MaiSeedanceModelSpec {
    // 每行依次描述模型 ID、展示名、最大生成时长、参考图上限、输入视频时长上限、
    // 是否支持 1080p/4k，以及是否使用 2.5 的 Omni 任务类型。
    const char* id;
    const char* label;
    int maximumDuration;
    int maximumReferenceImages;
    int maximumInputVideoDuration;
    bool supports1080p;
    bool supports4k;
    bool usesOmniTaskType;
};

constexpr MaiSeedanceModelSpec kSeedanceModels[] = {
    // 2.5 的规格保留供代码维护，但工具 schema 和执行入口会因费用将它禁用。
    {kMiniVideoModel, "Seedance 2.0 Mini", 15, 9, 15, false, false, false},
    {kFastVideoModel, "Seedance 2.0 Fast", 15, 9, 15, false, false, false},
    {kStandardVideoModel, "Seedance 2.0", 15, 9, 15, true, true, false},
    {kVideoModel, "Seedance 2.5", 30, 30, 30, true, false, true},
};

const MaiSeedanceModelSpec* seedanceModelSpec(const std::string& modelId) {
    for (const auto& model : kSeedanceModels) {
        if (modelId == model.id) return &model;
    }
    return nullptr;
}

std::string stringValue(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) return {};
    return object[key].get<std::string>();
}

MaiToolResult discoverSpecialist(const MaiSpecialistInfo& info, const std::string& reply) {
    // 模型理论能力、API 开放情况、工具真正接线状态分开返回；
    // limitation 原样保留具体限制，让主模型不会把“支持”理解成“任意输入都可用”。
    Json capabilities = Json::array();
    for (const MaiSpecialistCapability& capability : info.capabilities) {
        capabilities.push_back(
            Json{{"id", capability.id},
                 {"model_support", capability.modelSupported},
                 {"api_support", capability.apiSupported},
                 {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                 {"limitation", capability.limitation}});
    }
    return MaiToolResult::success(Json{
        {"tool_kind", "model_backed"},
        {"bound_model", info.modelId},
        {"configured", info.configured},
        {"reply", reply},
        {"capabilities",
         std::move(capabilities)}}.dump());
}

MaiToolResult fail(MaiErrorCode error, const char* code, const std::string& message) {
    return MaiToolResult::failure(error, Json{{"code", code}, {"message", message}}.dump());
}

MaiToolResult invalid(const std::string& message) {
    return fail(MaiErrorCode::InvalidInput, "invalid_input", message);
}

bool isHttpsUrl(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0) return false;
    CURLU* parsed = curl_url();
    if (parsed == nullptr) return false;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK;
    char* host = nullptr;
    const bool hasHost = valid && curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                         host != nullptr && *host != '\0';
    if (host != nullptr) curl_free(host);
    curl_url_cleanup(parsed);
    return hasHost;
}

bool validTaskId(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char value) {
               return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
                      (value >= 'a' && value <= 'z') || value == '-' || value == '_';
           });
}

std::string lowerExtension(const std::string& path) {
    const std::string base = MaiFilePath::fromUtf8(path).baseName().toUtf8();
    const std::size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string extension = base.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

std::string imageMime(const std::string& path) {
    const std::string extension = lowerExtension(path);
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "png") return "image/png";
    return {};
}

std::optional<MaiToolResult> validateLocalAssetImage(const std::string& candidate,
                                                     const MaiToolContext& context) {
    const std::string path = context.resolvePath(candidate);
    const MaiFilePath source = MaiFilePath::fromUtf8(path);
    std::uint64_t bytes = 0;
    if (path.empty() || imageMime(path).empty() || MaiFileSystem::isSymbolicLink(source) ||
        !MaiFileSystem::fileSize(source, bytes) || bytes == 0 || bytes > 30'000'000)
        return invalid("Local image must be an accessible JPEG or PNG under 30 MB");
    return std::nullopt;
}

std::optional<MaiToolResult> validateLocalAssetVideo(const std::string& candidate,
                                                     const MaiToolContext& context) {
    const std::string path = context.resolvePath(candidate);
    const MaiFilePath source = MaiFilePath::fromUtf8(path);
    const std::string extension = lowerExtension(path);
    std::uint64_t bytes = 0;
    if (path.empty() || (extension != "mp4" && extension != "mov") ||
        MaiFileSystem::isSymbolicLink(source) || !MaiFileSystem::fileSize(source, bytes) ||
        bytes == 0 || bytes > 200'000'000)
        return invalid("Local video must be an accessible MP4 or MOV under 200 MB");
    return std::nullopt;
}

struct ArkResponse {
    Json data;
    std::optional<MaiToolResult> error;
};

struct ResponseBuffer {
    std::string bytes;
    bool exceeded = false;
};

std::size_t receiveResponse(char* data, std::size_t size, std::size_t count, void* user) {
    auto& buffer = *static_cast<ResponseBuffer*>(user);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const std::size_t bytes = size * count;
    if (bytes > kMaxResponseBytes - buffer.bytes.size()) {
        buffer.exceeded = true;
        return 0;
    }
    buffer.bytes.append(data, bytes);
    return bytes;
}

int checkCanceled(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const MaiToolContext*>(user)->isCanceled() ? 1 : 0;
}

// 方舟请求统一处理认证、取消、状态码和 JSON 解析；查询异常与生成任务失败分开报告。
ArkResponse requestJson(const std::string& url, const std::string& key, const std::string& caBundle,
                        const Json* body, const MaiToolContext& context,
                        bool deleteRequest = false) {
    maiAssertBlockingAllowed("ark_media_tool");
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
        return {{}, fail(MaiErrorCode::Internal, "internal", "Could not initialize HTTP")};
    ResponseBuffer buffer;
    const std::string payload = body != nullptr ? body->dump() : std::string{};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + key).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    if (body != nullptr) headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, body != nullptr ? 180L : 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, checkCanceled);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
    if (body != nullptr) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(payload.size()));
    }
    if (deleteRequest) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (context.isCanceled())
        return {{}, fail(MaiErrorCode::Canceled, "canceled", "Task was canceled")};
    if (buffer.exceeded)
        return {{}, fail(MaiErrorCode::Protocol, "protocol", "Ark response was too large")};
    if (result != CURLE_OK)
        return {{}, fail(MaiErrorCode::Network, "network", curl_easy_strerror(result))};
    Json object = Json::parse(buffer.bytes, nullptr, false);
    if (!object.is_object())
        return {{}, fail(MaiErrorCode::Protocol, "protocol", "Ark returned invalid JSON")};
    if (status < 200 || status >= 300) {
        const Json detail = object.value("error", Json::object());
        const std::string message = stringValue(detail, "message");
        return {{},
                fail(MaiErrorCode::Network, "ark_error",
                     message.empty() ? "Ark request failed (HTTP " + std::to_string(status) + ")"
                                     : message)};
    }
    return {std::move(object), std::nullopt};
}

std::optional<MaiToolResult> readImage(const std::string& candidate, const MaiToolContext& context,
                                       const MaiCreativeMediaUploadProvider& uploadMedia,
                                       Json& image) {
    const std::string path = context.resolvePath(candidate);
    const std::string mime = imageMime(path);
    if (path.empty() || mime.empty())
        return invalid("image_path must be an accessible supported image");
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0 ||
        size > 20'000'000)
        return invalid("image_path must contain 1 to 20000000 bytes");
    // 图片只作为上传源路径；方舟收到 OSS 短期 HTTPS 地址，不能退回 Base64 直传。
    if (!uploadMedia)
        return fail(MaiErrorCode::NotConfigured, "upload_not_configured",
                    "Private OSS media upload is not configured");
    const auto uploaded = uploadMedia(path, context);
    if (!uploaded)
        return fail(uploaded.error().code(), "upload_failed", uploaded.error().message());
    if (!isHttpsUrl(uploaded.value()))
        return fail(MaiErrorCode::Protocol, "upload_failed", "OSS returned no HTTPS media URL");
    image = uploaded.value();
    return std::nullopt;
}

std::optional<MaiToolResult> outputName(const std::string& requested, const std::string& fallback,
                                        const char* extension, const MaiToolContext& context,
                                        std::string& relative) {
    relative = requested.empty() ? fallback : requested;
    if (context.root.empty() || MaiFilePath::fromUtf8(relative).isAbsolute() ||
        lowerExtension(relative) != extension ||
        maiResolvePathWithinRoot(context.root, relative).empty())
        return invalid(std::string("output_path must be a relative .") + extension +
                       " path inside the Agent workspace");
    return std::nullopt;
}

MaiToolResult downloadResult(const std::string& url, const std::string& relative, int maxSizeMb,
                             const std::string& caBundle, const MaiToolContext& context, bool png) {
    if (!isHttpsUrl(url))
        return fail(MaiErrorCode::Protocol, "protocol", "Ark returned no HTTPS media URL");
    auto downloader = makeMaiDownloadFileTool(caBundle);
    const Json arguments = {
        {"url", url}, {"output_path", relative}, {"max_size_mb", maxSizeMb}, {"timeout_s", 180}};
    MaiToolResult result = downloader->execute(arguments.dump(), context);
    if (result.hasError()) return result;
    const Json downloaded = Json::parse(result.output(), nullptr, false);
    const std::string path = stringValue(downloaded, "path");
    std::string prefix;
    const MaiError readError = MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), prefix, 12);
    const bool valid =
        !readError && (png ? prefix.size() >= 8 && prefix.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0
                           : prefix.size() >= 8 && prefix.compare(4, 4, "ftyp") == 0);
    if (!valid) {
        if (!path.empty()) (void)MaiFileSystem::removeFile(MaiFilePath::fromUtf8(path));
        return fail(MaiErrorCode::Protocol, "protocol", "Ark returned invalid media bytes");
    }
    Json output = downloaded;
    output["mime_type"] = png ? "image/png" : "video/mp4";
    return MaiToolResult::success(output.dump());
}

class MaiSeedanceVideoTool final : public MaiTool {
public:
    MaiSeedanceVideoTool(MaiArkApiKeyProvider provider, std::string caBundle,
                         MaiArkAssetProvider assets, MaiVideoMetadataProvider videoMetadata)
        : mKey(std::move(provider)),
          mCaBundle(std::move(caBundle)),
          mAssets(std::move(assets)),
          mVideoMetadata(std::move(videoMetadata)) {}

    std::string name() const override {
        return "seedance_video";
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (args.is_object() && stringValue(args, "model") == kVideoModel) return false;
        if (args.is_object() && (stringValue(args, "action") == "delegate" ||
                                 stringValue(args, "action") == "revise")) {
            if (args.contains("video_url")) return false;
            const bool hasLocalMedia = !stringValue(args, "video_path").empty() ||
                                       !stringValue(args, "image_path").empty() ||
                                       !stringValue(args, "last_frame_path").empty() ||
                                       (args.value("local_image_paths", Json::array()).is_array() &&
                                        !args.value("local_image_paths", Json::array()).empty());
            if (hasLocalMedia) return false;
            const std::string mode = stringValue(args, "mode");
            if (mode == "reference" || mode == "edit" || mode == "extend") {
                const int sourceCount =
                    static_cast<int>(!stringValue(args, "video_path").empty()) +
                    static_cast<int>(!stringValue(args, "video_task_id").empty()) +
                    static_cast<int>(!stringValue(args, "video_asset_id").empty());
                if (sourceCount != 1) return false;
            }
        }
        return isPaidGenerationAction(raw);
    }
    bool requiresPaidApproval(const std::string& raw) const override {
        return isBillableSubmissionAction(raw);
    }
    // 描述这个媒体专家的能力、限制和调用身份，供委托路由使用。
    // 声明内容必须与实际执行入口一致，避免模型选择不存在的能力。
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        // discover 按实际配置报告能力：Mini 为默认型号，Fast/标准版可选；2.5 因费用禁用。
        const bool configured = mKey && !mKey().empty();
        const auto unverified = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                           : MaiSpecialistCapabilityStatus::NotConfigured;
        const auto localMediaStatus = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                      : mAssets
                                          ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                          : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        const auto localVideoStatus = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                      : mAssets && mVideoMetadata
                                          ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                          : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        return MaiSpecialistInfo{
            name(),
            kDefaultVideoModel,
            configured,
            // 文字生成和三档 2.0 型号的时长/分辨率写进 discover；
            // “已接线待验证”不等于当前账号已通过所有付费实测。
            // text_to_video：默认 Mini 可生成 4–15 秒；Fast/标准版需显式选 model。
            // 2.5 已禁用，每个型号的账号权益仍需实盘验证。
            {{"text_to_video", true, true, unverified,
              "Default Seedance 2.0 Mini supports 4-15 seconds. Fast and standard 2.0 are "
              "available with model. Seedance 2.5 is disabled. "
              "Each model entitlement needs live validation"},
             // seedance_2_0_fast：4–15 秒、480p/720p，最多九张参考图。
             {"seedance_2_0_fast", true, true, unverified,
              "2.0 Fast supports 4-15 seconds, 480p/720p, and up to 9 reference images"},
             // seedance_2_0_mini：4–15 秒、480p/720p，最多九张参考图。
             {"seedance_2_0_mini", true, true, unverified,
              "2.0 Mini supports 4-15 seconds, 480p/720p, and up to 9 reference images"},
             // seedance_2_0_standard：4–15 秒、480p/720p/1080p/4k，最多九张图。
             {"seedance_2_0_standard", true, true, unverified,
              "2.0 supports 4-15 seconds, 480p/720p/1080p/4k, and up to 9 reference images"},
             // first_frame_to_video：Mini 可接首帧；付费前确认输出规格。
             {"first_frame_to_video", true, true, unverified,
              "Seedance 2.0 Mini accepts an Active Image asset as first_frame_asset_id; confirm "
              "the output settings before paying"},
             // video_edit_from_seedance_task：用已完成的 Seedance 任务作为编辑源。
             {"video_edit_from_seedance_task", true, true, unverified,
              "mode=edit accepts one completed Seedance video_task_id from the same provider; "
              "the referenced task must be queryable"},
             {"video_reference_from_asset", true, true, unverified,
              "An Active Video asset ID or one completed Seedance video_task_id can be the "
              "only source in reference, edit, or extend"},
             // first_and_last_frame_to_video：首尾帧必须是 Active 图片资产。
             {"first_and_last_frame_to_video", true, true, unverified,
              "mode=create accepts Active Image assets for first_frame_asset_id and optional "
              "last_frame_asset_id; strict frames cannot mix with reference images"},
             // platform_virtual_avatar：使用 ark_assets 或体验中心已有的 Active AIGC ID；
             // Ark 仍会审核上传内容，入库不等于自动过审。
             {"platform_virtual_avatar", true, true, unverified,
              "Use an Active AIGC asset ID from ark_assets or the Ark Experience Center; "
              "Ark validates uploaded content"},
             // authorized_real_portrait：使用同一 Ark 账号已授权的真人素材 ID；
             // 直接把手机本地真人脸照作为参考图提交，仍不属于这条授权路径。
             {"authorized_real_portrait", true, true, unverified,
              "Use an authorized real-person asset ID from the same Ark account; direct local "
              "face uploads remain unsupported"},
             // real_portrait_h5_registration：先生成 H5 本人验证链接；本人完成后查 GroupId，
             // 再使用该组中匹配且状态为 Active 的素材。
             {"real_portrait_h5_registration", true, true, unverified,
              "ark_assets can create an H5 verification link and query its GroupId after the "
              "person finishes; use matching Active assets from that group"},
             // local_portrait_asset_registration：由 ark_assets 准备素材，Seedance 只收 ID。
             {"local_portrait_asset_registration", true, true, localMediaStatus,
              "Local images are registered through ark_assets and must become Active before "
              "the paid task. Supply an existing asset_group_id and its asset_group_type; "
              "natural-person likenesses require a verified LivenessFace group"},
             // multi_reference_video：1–9 张图必须是 Active 资产 ID；
             // 参考模式不能混严格首尾帧，视频参考最多一个。
             {"multi_reference_video", true, true, unverified,
              "Up to nine Active Image assets through reference_asset_ids. References cannot "
              "mix with strict first/last-frame control. At most one video source is wired"},
             {"video_edit_from_local_file", true, true, localVideoStatus,
              "Local MP4/MOV is registered as a Video asset and must become Active before a "
              "paid edit, extend, or reference task. Supply asset_group_id and asset_group_type"},
             // 多段视频参考只在供应商文档里出现，当前适配器没有对应输入数组。
             {"multiple_reference_videos", false, false,
              MaiSpecialistCapabilityStatus::NotImplemented,
              "This tool accepts exactly one video source, not a list of videos"},
             // 管理动作和生成动作的条件不同，运行中的任务不能当作可取消。
             {"task_cancel_delete", false, true, unverified,
              "cancel applies only to queued tasks; delete removes delivered succeeded, "
              "failed, or expired Ark records. Running tasks cannot be canceled or deleted"}}};
    }
    // 构造模型可见的工具说明和参数约束。
    // 字段名及可选值须与执行入口的真实校验保持一致。
    std::string description() const override {
        if (!specialistInfo()->configured)
            return "Seedance video specialist defaults to doubao-seedance-2-0-mini-260615. Ark "
                   "API Key is not configured on this device. Use discover for capabilities.";
        // prepare 是非付费素材准备，返回只含 ID 的参数；delegate 才走逐次付费确认。
        // 已完成的 Seedance 任务沿用供应商任务 ID；外部视频 URL 不作为新输入。
        return "Seedance video through Ark. Use discover for capabilities, prepare for local "
               "media, delegate for a new paid task, revise for a completed task in this AI "
               "session, and continue to check status. Select Seedance 2.0 Mini (default), Fast, "
               "or standard 2.0 before paying; "
               "2.5 is disabled. Confirm duration, ratio, resolution, and audio. Mini and Fast "
               "support 4-15 seconds at 480p/720p; standard 2.0 also supports 1080p/4k. "
               "mode=create accepts text, up to nine reference images, or strict first and "
               "optional last frames. reference_asset_ids, first_frame_asset_id, and "
               "last_frame_asset_id must be Active Image assets. Strict frames cannot mix with "
               "reference images. mode=reference, edit, or extend requires exactly one source: "
               "video_asset_id or a completed Seedance video_task_id for paid submission; "
               "prepare also accepts local video_path. "
               "Direct video_url input is unsupported. Before paid delegate with "
               "local image_path, last_frame_path, local_image_paths, or video_path, call "
               "action=prepare with an existing asset_group_id and asset_group_type. Each "
               "prepare call registers at most one local file and returns prepared_arguments "
               "with its asset ID. If status is processing, call prepare again with "
               "those prepared_arguments; it will not upload the same file twice. Delegate "
               "rejects local paths and sends only asset:// IDs to Seedance. AIGC groups are for "
               "fictional "
               "subjects and must not be used for natural-person likenesses. A real person needs "
               "a LivenessFace group after that person completes Ark H5 verification; do not "
               "infer group type from the file. Existing asset IDs are checked for Active status "
               "and Image or Video type before submission. Registration failure, cancellation, "
               "or a processing timeout stops before the paid video request. The app checks "
               "submitted tasks automatically; continue is for manual diagnostics. Cancel can "
               "stop only queued tasks, and delete removes only delivered ended task records. "
               "Use agent_send_media to deliver a completed video.";
    }
    std::string parametersSchema() const override {
        // JSON Schema 只定义字段形状；execute 还会核对模式组合、文件存在、数量、
        // 付费规格与供应商能力。不能因为 schema 接受字段就假定某个组合可用。
        const std::string raw =
            R"({"type":"object","properties":{)"
            // action 为能力查询、提交、跟进、云端取消、云端删除或修订；
            // cancel/delete 要求当前会话的 conversation_id，不能传任意云端 ID。
            R"("action":{"type":"string","enum":["discover","prepare","delegate",)"
            R"("continue","cancel","delete","revise"]},)"
            // model 是 2.0 Mini/Fast/标准版三选一；2.5 故意不列入可提交值。
            R"("model":{"type":"string","enum":["doubao-seedance-2-0-mini-260615",)"
            R"("doubao-seedance-2-0-fast-260128","doubao-seedance-2-0-260128"]},)"
            // message 是视频要求；context 只带相关背景，修订时用 conversation_id。
            R"("message":{"type":"string"},"context":{"type":"string"},)"
            R"("conversation_id":{"type":"string"},"mode":{"type":"string",)"
            // create 生成；edit 改已有视频；extend 续写；reference 参考已有视频。
            R"("enum":["create","edit","extend","reference"]},)"
            // 严格首尾帧可分别使用本地路径或 Active 图片资产，不与任意参考图混用。
            R"("image_path":{"type":"string"},"last_frame_path":{"type":"string"},)"
            R"("first_frame_asset_id":{"type":"string"},)"
            R"("last_frame_asset_id":{"type":"string"},)"
            // 两种单人资产入口互斥；前者是虚拟素材，后者需要平台真人授权。
            R"("virtual_avatar_asset_id":{"type":"string"},)"
            R"("authorized_portrait_asset_id":{"type":"string"},)"
            // local_image_paths 是 OSS 上传源；reference_asset_ids 传 Active
            // 的资产 ID，可一次提交多张；不能把 asset:// 当本地 path 传入。
            R"("local_image_paths":{"type":"array","items":{"type":"string"}},)"
            R"("reference_asset_ids":{"type":"array","items":{"type":"string"},"maxItems":9},)"
            // 本地素材登记必须指定已有组及其类型；不接受直接视频 HTTPS 外链。
            R"("asset_group_id":{"type":"string"},)"
            R"("asset_group_type":{"type":"string","enum":["AIGC","LivenessFace"]},)"
            // 视频源可来自本地文件、旧任务或 Active 视频资产。
            R"("video_path":{"type":"string"},)"
            R"("video_task_id":{"type":"string"},"video_asset_id":{"type":"string"},)"
            R"("source_duration_seconds":{"type":"number"},"source_fps":{"type":"number"},)"
            R"("poll_once":{"type":"boolean"},)"
            // production 里的时长、画幅、分辨率和音频开关是付费提交的明确规格。
            R"("production":{"type":"object",)"
            R"("properties":{"duration":{"type":"integer"},"ratio":{"type":"string"},)"
            R"("resolution":{"type":"string"},"generate_audio":{"type":"boolean"}},)"
            R"("additionalProperties":false}},"required":["action"],)"
            R"("additionalProperties":false})";
        Json schema = Json::parse(raw);
        Json& fields = schema["properties"];
        // 以下英文逐字段送给主模型，中文逐段解释同一规则，方便维护者核对。
        // action：discover 只读；delegate 新建付费任务；continue 只查询；
        // revise 基于同一会话完成任务再次付费；cancel/delete 还要看供应商任务状态。
        fields["action"]["description"] =
            "discover is read-only; prepare registers or checks assets without video generation; "
            "delegate creates a paid task; continue polls; revise "
            "creates a paid edit from a completed task; cancel/delete require a saved local "
            "conversation_id and provider-supported state";
        // model：默认 Mini；Mini/Fast 只支持 480p/720p，标准版另支持 1080p/4k；
        // 三款 2.0 型号均为 4–15 秒。2.5 因费用禁用，不能写入可选枚举。
        fields["model"]["description"] =
            "2.0 Mini is default. Mini/Fast: 4-15 seconds, 480p/720p. Standard: 4-15 seconds, "
            "480p/720p/1080p/4k. Seedance 2.5 is disabled";
        // mode：create 不带源视频；reference/edit/extend 必须从本地文件、
        // 旧 Seedance 任务或视频资产中恰好选一个视频源。
        fields["mode"]["description"] =
            "create has no source video; reference/edit/extend require exactly one source in "
            "video_path, video_task_id, or video_asset_id";
        // message 写视频目标，context 写必要背景，不能把素材字节或全量聊天历史塞进去。
        fields["message"]["description"] = "Video creation or edit instruction for this task";
        fields["context"]["description"] =
            "Only relevant constraints and background; not media bytes or full chat history";
        // 严格首尾帧可以分别用本地路径或 Active 图片资产；尾帧必须配首帧，
        // 这条控制路径和任意多图参考不能混用。
        fields["image_path"]["description"] =
            "Local strict first frame; registered as an Ark Image asset before paid create";
        fields["last_frame_path"]["description"] =
            "Local strict last frame; registered as an Ark Image asset; requires a first frame";
        fields["first_frame_asset_id"]["description"] =
            "Active Image asset ID (asset://ID or bare ID) for the strict first frame; create "
            "mode only, mutually exclusive with image_path";
        fields["last_frame_asset_id"]["description"] =
            "Active Image asset ID (asset://ID or bare ID) for the strict last frame; requires "
            "a first frame, mutually exclusive with last_frame_path";
        // local_image_paths 是待上传 OSS 的普通本地图；reference_asset_ids 是
        // 已在方舟入库且状态 Active 的资产 ID，不能把 asset:// 当成本地文件名。
        // 两组图片合计最多九张；真人素材应按用户选定的资产/授权路径引用。
        fields["local_image_paths"]["description"] =
            "Local reference images registered as Ark Image assets before paid generation; "
            "combined with reference_asset_ids, at most nine total";
        fields["local_image_paths"]["maxItems"] = 9;
        fields["reference_asset_ids"]["description"] =
            "Active Ark asset IDs (asset://ID or bare ID); combined with local_image_paths, "
            "at most nine total. Never pass these IDs as local_image_paths";
        // 两种单资产字段互斥：虚拟 AIGC 素材须 Active；真人肖像要先完成
        // 对应本人 H5 验证并引用已授权素材，不能靠上传一张普通脸照替代授权。
        fields["virtual_avatar_asset_id"]["description"] =
            "One Active AIGC virtual asset ID after ark_assets registration; mutually "
            "exclusive with authorized_portrait_asset_id";
        fields["authorized_portrait_asset_id"]["description"] =
            "One authorized real-portrait asset ID after the person's H5 validation; "
            "mutually exclusive with virtual_avatar_asset_id";
        // 视频源三选一：video_path 是本地 MP4/MOV 注册源，须先用 ffprobe
        // 核对 2–15 秒及 24–60 fps；
        // video_task_id 必须是可读取且已完成的方舟任务；video_asset_id 是 Active 资产。
        fields["video_path"]["description"] =
            "One local MP4/MOV registered as an Ark Video asset before paid generation; check "
            "2-15 seconds and 24-60 fps";
        fields["source_duration_seconds"]["description"] =
            "Measured and filled by prepare for local video_path; callers must not estimate it";
        fields["source_fps"]["description"] =
            "Measured and filled by prepare for local video_path; callers must not estimate it";
        fields["asset_group_id"]["description"] =
            "Existing Ark group for every local image or video supplied in this call";
        fields["asset_group_type"]["description"] =
            "Actual type of asset_group_id: AIGC for fictional subjects or LivenessFace after "
            "the real person completes verification; never infer this from the media";
        fields["video_task_id"]["description"] =
            "One completed Seedance provider task ID used as source video";
        fields["video_asset_id"]["description"] =
            "One Active Video asset ID (asset://ID or bare ID) used as source video in "
            "reference, edit, or extend mode";
        // production 是本次付费任务的明确规格：输出时长、画幅、分辨率与声音。
        // duration 可以用 -1 交由平台自动决定；具体分辨率仍要受所选型号限制。
        fields["production"]["description"] =
            "Confirm output duration, ratio, resolution, and audio before paid generation";
        Json& output = fields["production"]["properties"];
        output["duration"]["description"] =
            "Output seconds 4-15 for Seedance 2.0, or -1 for provider automatic duration";
        output["ratio"]["enum"] =
            Json::array({"adaptive", "16:9", "9:16", "1:1", "4:3", "3:4", "21:9"});
        output["ratio"]["description"] =
            "Confirmed output aspect ratio or adaptive according to the selected input";
        output["resolution"]["enum"] = Json::array({"480p", "720p", "1080p", "4k"});
        output["resolution"]["description"] =
            "Mini/Fast allow 480p/720p; standard 2.0 additionally allows 1080p/4k";
        output["generate_audio"]["description"] = "Whether the video generates audio";
        // conversation_id 是当前会话保存的 spt_ ID，供继续、修订和任务管理；
        // poll_once 仅查一次进度，正常情况下 App 后台会自动查询并交付。
        fields["conversation_id"]["description"] =
            "Saved spt_ task ID for continue, revise, cancel, or delete in this AI conversation";
        fields["poll_once"]["description"] =
            "For prepare, check assets once and return processing IDs if pending; for continue, "
            "query video status once. The App normally follows submitted video tasks";
        return schema.dump();
    }
    // 解析模型提供的参数并提交或查询方舟媒体任务。
    // 成功和失败都转成明确的工具结果；中间状态不能冒充最终产物。
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        // Schema 已撤掉旧多图字段；执行层也拒绝未知参数，避免旧调用静默丢图后付费。
        const Json schema = Json::parse(parametersSchema(), nullptr, false);
        for (auto field = args.begin(); field != args.end(); ++field) {
            if (!schema["properties"].contains(field.key()))
                return invalid("unsupported Seedance parameter: " + field.key());
        }
        for (const char* field :
             {"action", "model", "message", "context", "conversation_id", "mode", "image_path",
              "last_frame_path", "first_frame_asset_id", "last_frame_asset_id",
              "virtual_avatar_asset_id", "authorized_portrait_asset_id", "video_path",
              "video_task_id", "video_asset_id", "asset_group_id", "asset_group_type"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        if (args.contains("local_image_paths") && !args["local_image_paths"].is_array())
            return invalid("local_image_paths must be an array");
        if (args.contains("reference_asset_ids") && !args["reference_asset_ids"].is_array())
            return invalid("reference_asset_ids must be an array");
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return invalid("poll_once must be a boolean");
        for (const char* field : {"source_duration_seconds", "source_fps"}) {
            if (args.contains(field) && !args[field].is_number())
                return invalid(std::string(field) + " must be a number");
        }
        const std::string action = stringValue(args, "action");
        const std::string message = stringValue(args, "message");
        if (message.empty() && (action == "delegate" || action == "revise"))
            return invalid("message is required to start or revise a task");
        if (action == "discover") {
            // 发现能力只读：返回默认型号、可选型号、规格和账号实盘验证状态，绝不生成。
            const MaiToolResult base = discoverSpecialist(
                *specialistInfo(),
                "Choose Seedance 2.0 Mini (default), 2.0 Fast, or standard 2.0 with model "
                "before paid generation. Seedance 2.5 is disabled. "
                "Local media requires an Ark Assets provider plus asset_group_id and "
                "asset_group_type; registration must reach Active before a paid task.");
            Json info = Json::parse(base.output());
            Json models = Json::array();
            for (const auto& model : kSeedanceModels) {
                if (std::string(model.id) == kVideoModel) continue;
                models.push_back(Json{
                    {"id", model.id},
                    {"label", model.label},
                    {"duration_seconds", {{"minimum", 4}, {"maximum", model.maximumDuration}}},
                    {"resolutions", model.supports4k ? Json::array({"480p", "720p", "1080p", "4k"})
                                    : model.supports1080p ? Json::array({"480p", "720p", "1080p"})
                                                          : Json::array({"480p", "720p"})},
                    {"maximum_reference_images", model.maximumReferenceImages},
                    {"entitlement_verified", false}});
            }
            info["models"] = std::move(models);
            return MaiToolResult::success(info.dump());
        }
        if (action == "prepare") return prepare(args, context);
        const std::string key = mKey ? mKey() : std::string{};
        if (key.empty())
            return fail(MaiErrorCode::NotConfigured, "not_configured",
                        "Configure an Ark API key in this platform's Agent settings");
        // delegate 新建付费任务；continue 只查已提交任务，不能再次生成。
        if (action == "delegate") return delegate(args, key, context);
        if (action == "continue") return continueTask(args, key, context);
        if (action == "cancel" || action == "delete") return manageTask(args, key, context);
        if (action == "revise") {
            // 修订要先核对旧任务属于当前 AI 会话，再继承原目标和素材。
            if (context.specialistTasks == nullptr)
                return fail(MaiErrorCode::NotConfigured, "not_configured",
                            "Specialist task history is unavailable");
            const std::string previousId = stringValue(args, "conversation_id");
            MaiSpecialistTask previous;
            if (!context.specialistTasks->getSpecialistTask(previousId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name() || previous.providerTaskId.empty())
                return fail(MaiErrorCode::NotFound, "not_found",
                            "Previous Seedance task was not found in this AI conversation");
            if (!stringValue(args, "video_path").empty() ||
                !stringValue(args, "video_task_id").empty() ||
                !stringValue(args, "video_asset_id").empty())
                return invalid("revise uses the previous task video; do not override its source");
            if (!validTaskId(previous.providerTaskId))
                return invalid("previous Seedance task ID is invalid");
            ArkResponse priorTask =
                requestJson(std::string(kVideoTasksUrl) + "/" + previous.providerTaskId, key,
                            mCaBundle, nullptr, context);
            if (priorTask.error) return *priorTask.error;
            const std::string previousModel = stringValue(priorTask.data, "model");
            if (seedanceModelSpec(previousModel) == nullptr)
                return fail(MaiErrorCode::Protocol, "unsupported_model",
                            "Previous Seedance model is not available for revision");
            const std::string requestedModel = stringValue(args, "model");
            if (!requestedModel.empty() && requestedModel != previousModel)
                return invalid(
                    "revise must use the previous Seedance model; delegate a new task "
                    "to switch models");
            Json revision = args;
            revision["model"] = previousModel;
            revision["mode"] = "edit";
            revision["video_task_id"] = previous.providerTaskId;
            revision["parent_task_id"] = previous.id;
            std::string summary = previous.contextSummary;
            if (!summary.empty()) summary += "\n";
            summary += "Previous goal: " + previous.intent;
            const std::string current = stringValue(args, "context");
            if (!current.empty()) summary += "\nCurrent constraints: " + current;
            if (summary.size() > 4000)
                return invalid("Revision context is too long; summarize the prior goal");
            revision["context"] = std::move(summary);
            return delegate(revision, key, context);
        }
        return invalid(
            "action must be discover, prepare, delegate, continue, cancel, delete, "
            "or revise");
    }

private:
    std::optional<MaiToolResult> assetRequest(const Json& request, const MaiToolContext& context,
                                              Json& result) const {
        if (!mAssets)
            return fail(MaiErrorCode::NotConfigured, "assets_not_configured",
                        "Ark Assets service is required to prepare or verify media");
        if (context.isCanceled())
            return fail(MaiErrorCode::Canceled, "canceled", "Asset preparation was canceled");
        const MaiResult<std::string> response = mAssets(request.dump(), context);
        if (!response)
            return fail(response.error().code(), "asset_request_failed",
                        response.error().message());
        if (context.isCanceled())
            return fail(MaiErrorCode::Canceled, "canceled", "Asset preparation was canceled");
        result = Json::parse(response.value(), nullptr, false);
        if (!result.is_object())
            return fail(MaiErrorCode::Protocol, "asset_protocol",
                        "Ark Assets returned invalid JSON");
        return std::nullopt;
    }

    std::optional<MaiToolResult> checkAssetGroup(const std::string& groupId,
                                                 const std::string& groupType,
                                                 const MaiToolContext& context) const {
        Json group;
        if (auto error =
                assetRequest(Json{{"action", "get_group"}, {"group_id", groupId}}, context, group))
            return error;
        if (stringValue(group, "Id") != groupId || stringValue(group, "GroupType").empty())
            return fail(MaiErrorCode::Protocol, "asset_protocol",
                        "Ark did not return the requested asset group");
        if (stringValue(group, "GroupType") != groupType)
            return invalid("asset_group_type does not match the Ark asset group");
        return std::nullopt;
    }

    std::optional<MaiToolResult> readAssetStatus(const std::string& assetId,
                                                 const char* expectedType,
                                                 const MaiToolContext& context, std::string& status,
                                                 std::string* groupId = nullptr) const {
        Json asset;
        if (auto error =
                assetRequest(Json{{"action", "get_asset"}, {"asset_id", assetId}}, context, asset))
            return error;
        if (stringValue(asset, "Id") != assetId)
            return fail(MaiErrorCode::Protocol, "asset_protocol",
                        "Ark did not return the requested asset ID");
        status = stringValue(asset, "Status");
        if (groupId != nullptr) *groupId = stringValue(asset, "GroupId");
        const std::string actualType = stringValue(asset, "AssetType");
        if (!actualType.empty() && actualType != expectedType)
            return invalid(std::string("Ark asset ") + assetId + " is not a " + expectedType +
                           " asset");
        if (status == "Active" && actualType.empty())
            return fail(MaiErrorCode::Protocol, "asset_protocol",
                        "Ark returned an Active asset without its type");
        if (status == "Failed") {
            const Json detail = asset.value("Error", Json::object());
            const std::string providerCode = stringValue(detail, "Code");
            const std::string providerMessage = stringValue(detail, "Message");
            std::string message = "Ark asset " + assetId + " failed processing";
            if (!providerCode.empty()) message += " (" + providerCode + ")";
            if (!providerMessage.empty()) message += ": " + providerMessage;
            return fail(MaiErrorCode::InvalidInput, "asset_failed", message);
        }
        if (status != "Active" && status != "Processing" && status != "Pending")
            return fail(MaiErrorCode::Protocol, "asset_protocol",
                        "Ark returned an unknown asset status for " + assetId);
        return std::nullopt;
    }

    std::optional<MaiToolResult> checkActiveAsset(const std::string& assetId,
                                                  const char* expectedType,
                                                  const MaiToolContext& context,
                                                  const std::string& expectedGroupType = {},
                                                  const std::string& expectedGroupId = {}) const {
        std::string status;
        std::string groupId;
        if (auto error = readAssetStatus(assetId, expectedType, context, status, &groupId))
            return error;
        if (status != "Active")
            return invalid("Ark asset " + assetId + " is not Active; use prepare before paying");
        if (!expectedGroupId.empty() && groupId != expectedGroupId)
            return invalid("Ark asset " + assetId + " is not in the requested asset group");
        if (!expectedGroupType.empty()) {
            if (!validArkGroupId(groupId))
                return fail(MaiErrorCode::Protocol, "asset_protocol",
                            "Ark asset did not report a valid group ID");
            if (auto error = checkAssetGroup(groupId, expectedGroupType, context)) return error;
        }
        return std::nullopt;
    }

    std::optional<MaiToolResult> uploadLocalAsset(const std::string& path, const char* action,
                                                  const std::string& groupId,
                                                  const std::string& groupType,
                                                  const MaiToolContext& context,
                                                  std::string& assetId) const {
        Json request = {
            {"action", action}, {"group_id", groupId}, {"expected_group_type", groupType}};
        if (std::string(action) == "upload_video") {
            request["video_path"] = path;
        } else {
            request["image_path"] = path;
        }
        auto tool = makeMaiArkAssetTool(mAssets);
        const MaiToolResult uploaded = tool->execute(request.dump(), context);
        if (uploaded.hasError())
            return fail(uploaded.error().code(), "asset_upload_failed", uploaded.error().message());
        const Json result = Json::parse(uploaded.output(), nullptr, false);
        assetId = stringValue(result, "Id");
        if (arkAssetId(assetId).empty())
            return fail(MaiErrorCode::Protocol, "asset_protocol",
                        "Ark did not return an asset ID after upload");
        return std::nullopt;
    }

    MaiToolResult prepare(const Json& args, const MaiToolContext& context) const {
        const std::string modelId = stringValue(args, "model").empty()
                                        ? std::string(kDefaultVideoModel)
                                        : stringValue(args, "model");
        if (modelId == kVideoModel) return invalid("Seedance 2.5 is disabled");
        const MaiSeedanceModelSpec* model = seedanceModelSpec(modelId);
        if (model == nullptr) return invalid("unsupported Seedance model");
        const bool revision = !stringValue(args, "conversation_id").empty();
        const std::string mode = revision                            ? "edit"
                                 : stringValue(args, "mode").empty() ? "create"
                                                                     : stringValue(args, "mode");
        if (mode != "create" && mode != "edit" && mode != "extend" && mode != "reference")
            return invalid("unsupported video mode");
        const std::string imagePath = stringValue(args, "image_path");
        const std::string lastFramePath = stringValue(args, "last_frame_path");
        const std::string videoPath = stringValue(args, "video_path");
        const std::string firstInput = stringValue(args, "first_frame_asset_id");
        const std::string lastInput = stringValue(args, "last_frame_asset_id");
        const std::string videoInput = stringValue(args, "video_asset_id");
        const std::string avatarInput = stringValue(args, "virtual_avatar_asset_id");
        const std::string portraitInput = stringValue(args, "authorized_portrait_asset_id");
        const std::string groupId = stringValue(args, "asset_group_id");
        const std::string groupType = stringValue(args, "asset_group_type");
        const Json localImages = args.value("local_image_paths", Json::array());
        const Json referenceIds = args.value("reference_asset_ids", Json::array());
        const std::string taskId = stringValue(args, "video_task_id");
        if (!localImages.is_array() || !referenceIds.is_array())
            return invalid("image path and asset ID fields must be arrays");
        if (args.contains("video_url"))
            return invalid("video_url is unsupported; use a video asset");
        if (revision && (!videoPath.empty() || !taskId.empty() || !videoInput.empty()))
            return invalid("revise uses the previous task video; do not override its source");
        const int sourceCount = static_cast<int>(!videoPath.empty()) +
                                static_cast<int>(!taskId.empty()) +
                                static_cast<int>(!videoInput.empty());
        if (!revision &&
            ((mode == "create" && sourceCount != 0) || (mode != "create" && sourceCount != 1)))
            return invalid("provide one video source for edit, extend, or reference only");
        if (!taskId.empty() && !validTaskId(taskId)) return invalid("invalid video_task_id");
        if (!imagePath.empty() && !firstInput.empty())
            return invalid("choose either image_path or first_frame_asset_id");
        if (!lastFramePath.empty() && !lastInput.empty())
            return invalid("choose either last_frame_path or last_frame_asset_id");
        if (!firstInput.empty() && (arkAssetId(firstInput).empty() || mode != "create"))
            return invalid("first_frame_asset_id requires create mode and a valid asset ID");
        if (!lastInput.empty() && arkAssetId(lastInput).empty())
            return invalid("last_frame_asset_id must be a valid asset ID");
        if ((!lastFramePath.empty() || !lastInput.empty()) &&
            (mode != "create" || (imagePath.empty() && firstInput.empty())))
            return invalid("last frame requires create mode and a first frame");
        if (!videoInput.empty() && arkAssetId(videoInput).empty())
            return invalid("video_asset_id must be a valid asset ID");
        if (!avatarInput.empty() && !portraitInput.empty())
            return invalid("choose either a virtual avatar or an authorized portrait asset");
        if ((!avatarInput.empty() && arkAssetId(avatarInput).empty()) ||
            (!portraitInput.empty() && arkAssetId(portraitInput).empty()))
            return invalid("portrait asset must be a valid asset ID");
        if ((!avatarInput.empty() || !portraitInput.empty()) &&
            (!referenceIds.empty() || !localImages.empty() ||
             (mode != "create" && !imagePath.empty())))
            return invalid("use either a single portrait asset or reference_asset_ids");
        const std::size_t referenceCount =
            localImages.size() + referenceIds.size() +
            static_cast<std::size_t>(!avatarInput.empty() || !portraitInput.empty()) +
            static_cast<std::size_t>(mode != "create" && !imagePath.empty());
        if (referenceCount > static_cast<std::size_t>(model->maximumReferenceImages))
            return invalid("Seedance accepts at most 9 reference images");
        if (referenceCount != 0 &&
            (!lastFramePath.empty() || !lastInput.empty() || !firstInput.empty() ||
             (mode == "create" && !imagePath.empty())))
            return invalid("reference images cannot be mixed with strict first/last frames");
        for (const Json& value : referenceIds)
            if (!value.is_string() || arkAssetId(value.get<std::string>()).empty())
                return invalid("reference_asset_ids entries must be valid asset IDs");
        for (const Json& value : localImages) {
            if (!value.is_string() || value.get<std::string>().empty())
                return invalid("local_image_paths entries must be nonempty strings");
            if (auto error = validateLocalAssetImage(value.get<std::string>(), context))
                return *error;
        }
        if (!imagePath.empty()) {
            if (auto error = validateLocalAssetImage(imagePath, context)) return *error;
        }
        if (!lastFramePath.empty()) {
            if (auto error = validateLocalAssetImage(lastFramePath, context)) return *error;
        }
        std::optional<MaiVideoMetadata> measuredVideo;
        if (!videoPath.empty()) {
            if (auto error = validateLocalAssetVideo(videoPath, context)) return *error;
            if (!mVideoMetadata)
                return fail(MaiErrorCode::NotConfigured, "video_probe_not_configured",
                            "FFprobe video metadata is unavailable on this device");
            const MaiResult<MaiVideoMetadata> metadata = mVideoMetadata(videoPath, context);
            if (!metadata)
                return fail(metadata.error().code(), "video_probe_failed",
                            metadata.error().message());
            const double sourceDuration = metadata.value().durationSeconds;
            const double sourceFps = metadata.value().framesPerSecond;
            if (!std::isfinite(sourceDuration) || sourceDuration < 2 || sourceDuration > 15)
                return invalid("Seedance 2.0 source video must be 2-15 seconds");
            if (!std::isfinite(sourceFps) || sourceFps < 24 || sourceFps > 60)
                return invalid("Seedance source video must be 24-60 fps");
            measuredVideo = metadata.value();
        }
        const bool hasLocal = !localImages.empty() || !imagePath.empty() ||
                              !lastFramePath.empty() || !videoPath.empty();
        if ((hasLocal || !groupId.empty() || !groupType.empty()) &&
            (!validArkGroupId(groupId) || (groupType != "AIGC" && groupType != "LivenessFace")))
            return invalid("Valid asset_group_id and asset_group_type are required together");
        if (hasLocal) {
            if (auto error = checkAssetGroup(groupId, groupType, context)) return *error;
        }

        Json prepared = args;
        if (measuredVideo) {
            prepared["source_duration_seconds"] = measuredVideo->durationSeconds;
            prepared["source_fps"] = measuredVideo->framesPerSecond;
        }
        if (localImages.empty()) prepared.erase("local_image_paths");
        if (imagePath.empty()) prepared.erase("image_path");
        if (lastFramePath.empty()) prepared.erase("last_frame_path");
        if (videoPath.empty()) prepared.erase("video_path");
        Json normalizedRefs = Json::array();
        for (const Json& value : referenceIds)
            normalizedRefs.push_back(arkAssetId(value.get<std::string>()));
        if (!firstInput.empty()) prepared["first_frame_asset_id"] = arkAssetId(firstInput);
        if (!lastInput.empty()) prepared["last_frame_asset_id"] = arkAssetId(lastInput);
        if (!videoInput.empty()) prepared["video_asset_id"] = arkAssetId(videoInput);
        if (!avatarInput.empty()) prepared["virtual_avatar_asset_id"] = arkAssetId(avatarInput);
        if (!portraitInput.empty())
            prepared["authorized_portrait_asset_id"] = arkAssetId(portraitInput);
        std::vector<std::pair<std::string, const char*>> assets;
        if (!avatarInput.empty()) assets.emplace_back(arkAssetId(avatarInput), "Image");
        if (!portraitInput.empty()) assets.emplace_back(arkAssetId(portraitInput), "Image");
        for (const Json& value : normalizedRefs)
            assets.emplace_back(value.get<std::string>(), "Image");
        if (!firstInput.empty()) assets.emplace_back(arkAssetId(firstInput), "Image");
        if (!lastInput.empty()) assets.emplace_back(arkAssetId(lastInput), "Image");
        if (!videoInput.empty()) assets.emplace_back(arkAssetId(videoInput), "Video");

        Json pending = Json::array();
        const int maximumChecks = args.value("poll_once", false) ? 1 : 30;
        for (int attempt = 0; attempt < maximumChecks; ++attempt) {
            pending = Json::array();
            for (const auto& [id, type] : assets) {
                std::string status;
                if (auto error = readAssetStatus(id, type, context, status)) return *error;
                if (status != "Active") pending.push_back(id);
            }
            if (pending.empty()) break;
            if (attempt + 1 < maximumChecks) {
                for (int tick = 0; tick < 10; ++tick) {
                    if (context.isCanceled())
                        return fail(MaiErrorCode::Canceled, "canceled",
                                    "Asset preparation was canceled");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        if (!pending.empty()) {
            if (!normalizedRefs.empty())
                prepared["reference_asset_ids"] = std::move(normalizedRefs);
            prepared["action"] = "prepare";
            return MaiToolResult::success(
                Json{{"status", "processing"},
                     {"prepared_arguments", std::move(prepared)},
                     {"pending_asset_ids", std::move(pending)},
                     {"reply",
                      "Assets are still processing; call prepare again with the "
                      "prepared arguments. No video task was submitted."}}
                    .dump());
        }
        if (!avatarInput.empty()) {
            if (auto error = checkActiveAsset(arkAssetId(avatarInput), "Image", context, "AIGC"))
                return *error;
        }
        if (!portraitInput.empty()) {
            if (auto error =
                    checkActiveAsset(arkAssetId(portraitInput), "Image", context, "LivenessFace"))
                return *error;
        }
        if (!hasLocal && validArkGroupId(groupId) &&
            (groupType == "AIGC" || groupType == "LivenessFace")) {
            if (auto error = checkAssetGroup(groupId, groupType, context)) return *error;
            for (const auto& [id, type] : assets) {
                if (auto error = checkActiveAsset(id, type, context, {}, groupId)) return *error;
            }
        }

        std::string uploadedId;
        if (!localImages.empty()) {
            if (auto error =
                    uploadLocalAsset(localImages.front().get<std::string>(), "upload_image",
                                     groupId, groupType, context, uploadedId))
                return *error;
            normalizedRefs.push_back(uploadedId);
            Json remaining = localImages;
            remaining.erase(remaining.begin());
            if (remaining.empty())
                prepared.erase("local_image_paths");
            else
                prepared["local_image_paths"] = std::move(remaining);
        } else if (!imagePath.empty()) {
            if (auto error = uploadLocalAsset(imagePath, "upload_image", groupId, groupType,
                                              context, uploadedId))
                return *error;
            prepared.erase("image_path");
            if (mode == "create")
                prepared["first_frame_asset_id"] = uploadedId;
            else
                normalizedRefs.push_back(uploadedId);
        } else if (!lastFramePath.empty()) {
            if (auto error = uploadLocalAsset(lastFramePath, "upload_image", groupId, groupType,
                                              context, uploadedId))
                return *error;
            prepared.erase("last_frame_path");
            prepared["last_frame_asset_id"] = uploadedId;
        } else if (!videoPath.empty()) {
            if (auto error = uploadLocalAsset(videoPath, "upload_video", groupId, groupType,
                                              context, uploadedId))
                return *error;
            prepared.erase("video_path");
            prepared["video_asset_id"] = uploadedId;
        }
        if (!normalizedRefs.empty()) prepared["reference_asset_ids"] = std::move(normalizedRefs);
        const bool remainingLocal =
            prepared.contains("local_image_paths") || prepared.contains("image_path") ||
            prepared.contains("last_frame_path") || prepared.contains("video_path");
        if (!uploadedId.empty() || remainingLocal) {
            prepared["action"] = "prepare";
            return MaiToolResult::success(
                Json{{"status", "processing"},
                     {"prepared_arguments", std::move(prepared)},
                     {"pending_asset_ids",
                      uploadedId.empty() ? Json::array() : Json::array({uploadedId})},
                     {"reply",
                      "Asset preparation is in progress; call prepare again with the "
                      "prepared arguments. No video task was submitted."}}
                    .dump());
        }
        prepared["action"] = revision ? "revise" : "delegate";
        prepared.erase("poll_once");
        return MaiToolResult::success(
            Json{{"status", "ready"},
                 {"prepared_arguments", std::move(prepared)},
                 {"pending_asset_ids", Json::array()},
                 {"reply", "Assets are Active; submit the prepared arguments for paid approval."}}
                .dump());
    }

    // 仅管理当前会话已持久化的方舟任务，不能凭任意云任务 ID 修改其他用户任务。
    // 供应商操作与本地任务状态分别回报，避免只完成一侧却宣称全部成功。
    MaiToolResult manageTask(const Json& args, const std::string& key,
                             const MaiToolContext& context) const {
        // 只接受当前会话持久化的任务；不能凭任意云端 task_id 删除其他任务。
        const std::string conversationId = stringValue(args, "conversation_id");
        MaiSpecialistTask task;
        if (conversationId.compare(0, 4, "spt_") != 0 || context.specialistTasks == nullptr ||
            !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId, task) ||
            task.specialistName != name() || !validTaskId(task.providerTaskId))
            return fail(MaiErrorCode::NotFound, "not_found",
                        "Seedance task was not found in this AI conversation");
        const std::string action = stringValue(args, "action");
        if (action == "cancel" && task.status != MaiSpecialistTaskStatus::Submitted &&
            task.status != MaiSpecialistTaskStatus::Running)
            return invalid("Only an active queued video task can be canceled");
        if (action == "delete" &&
            (task.status == MaiSpecialistTaskStatus::Submitted ||
             task.status == MaiSpecialistTaskStatus::Running || task.notifiedAt == 0))
            return invalid("Delete requires a completed and delivered task");
        const std::string url = std::string(kVideoTasksUrl) + "/" + task.providerTaskId;
        ArkResponse current = requestJson(url, key, mCaBundle, nullptr, context);
        if (current.error) return *current.error;
        const std::string status = stringValue(current.data, "status");
        if (action == "cancel" && status != "queued")
            return invalid("Ark can cancel only queued tasks; this task has started or ended");
        if (action == "delete" && status != "succeeded" && status != "failed" &&
            status != "expired")
            return invalid("Ark can delete only succeeded, failed, or expired task records");
        if (action == "delete" && status == "succeeded" &&
            task.status != MaiSpecialistTaskStatus::Succeeded)
            return invalid(
                "Recover and deliver the completed video before deleting its cloud record");
        // 同一个 DELETE 端点按供应商当前状态选择取消或删除；成功前不改本地记录。
        ArkResponse changed = requestJson(url, key, mCaBundle, nullptr, context, true);
        if (changed.error) return *changed.error;
        Json result = {{"conversation_id", conversationId}, {"task_id", task.providerTaskId}};
        if (action == "cancel") {
            const MaiError saved = context.specialistTasks->finishSpecialistTask(
                conversationId, context.sessionId, MaiSpecialistTaskStatus::Canceled,
                "The queued Seedance video task was canceled by Ark.", {},
                MaiTime::getCurrentTime());
            result.update(Json{{"status", "cancelled"},
                               {"reply", "The queued cloud video task was canceled."}});
            if (saved) result["local_state_warning"] = saved.message();
        } else {
            const MaiError removed = context.specialistTasks->deleteCompletedSpecialistTask(
                conversationId, context.sessionId);
            result.update(Json{{"status", "deleted"},
                               {"provider_deleted", true},
                               {"reply", "Ark task record deleted; downloaded video remains."}});
            if (removed) result["local_state_warning"] = removed.message();
        }
        return MaiToolResult::success(result.dump());
    }

    // 校验 Seedance 输入并提交异步视频生成任务，保存供应商任务标识以供继续查询。
    // 付费请求一旦受理，后续持久化失败也必须把真实任务 ID 回给调用方，防止重提。
    MaiToolResult delegate(const Json& args, const std::string& key,
                           const MaiToolContext& context) const {
        if (args.contains("video_path") || args.contains("image_path") ||
            args.contains("last_frame_path") || args.contains("local_image_paths"))
            return invalid("Run action=prepare with local media before paid delegate or revise");
        // 第一阶段只做型号、文本长度和动作模式校验；费用较高的型号在这里被拒绝。
        const std::string modelId = stringValue(args, "model").empty()
                                        ? std::string(kDefaultVideoModel)
                                        : stringValue(args, "model");
        if (modelId == kVideoModel)
            return invalid(
                "Seedance 2.5 is disabled because of its cost; choose 2.0 Mini, "
                "2.0 Fast, or standard 2.0");
        const MaiSeedanceModelSpec* model = seedanceModelSpec(modelId);
        if (model == nullptr) return invalid("unsupported Seedance model");
        const std::string message = stringValue(args, "message");
        if (message.size() > 4000) return invalid("message must be at most 4000 characters");
        const std::string extra = stringValue(args, "context");
        if (extra.size() > 4000) return invalid("context must be at most 4000 characters");
        const std::string mode =
            stringValue(args, "mode").empty() ? "create" : stringValue(args, "mode");
        if (mode != "create" && mode != "edit" && mode != "extend" && mode != "reference")
            return invalid("unsupported video mode");
        const std::string imagePath = stringValue(args, "image_path");
        const std::string lastFramePath = stringValue(args, "last_frame_path");
        const std::string firstFrameAssetInput = stringValue(args, "first_frame_asset_id");
        const std::string lastFrameAssetInput = stringValue(args, "last_frame_asset_id");
        const std::string firstFrameAssetId =
            firstFrameAssetInput.empty() ? std::string{} : arkAssetId(firstFrameAssetInput);
        const std::string lastFrameAssetId =
            lastFrameAssetInput.empty() ? std::string{} : arkAssetId(lastFrameAssetInput);
        const std::string avatarAssetInput = stringValue(args, "virtual_avatar_asset_id");
        const std::string authorizedAssetInput = stringValue(args, "authorized_portrait_asset_id");
        const std::string selectedAssetInput =
            avatarAssetInput.empty() ? authorizedAssetInput : avatarAssetInput;
        const std::string selectedAssetId =
            selectedAssetInput.empty() ? std::string{} : arkAssetId(selectedAssetInput);
        const Json referenceImagePaths = args.value("local_image_paths", Json::array());
        const Json referenceAssetIds = args.value("reference_asset_ids", Json::array());
        const std::string videoPath = stringValue(args, "video_path");
        const std::string videoTaskId = stringValue(args, "video_task_id");
        const std::string videoAssetInput = stringValue(args, "video_asset_id");
        const std::string videoAssetId =
            videoAssetInput.empty() ? std::string{} : arkAssetId(videoAssetInput);
        const std::string assetGroupId = stringValue(args, "asset_group_id");
        const std::string assetGroupType = stringValue(args, "asset_group_type");
        const bool hasGroupConstraint = !assetGroupId.empty() || !assetGroupType.empty();
        if (hasGroupConstraint && (!validArkGroupId(assetGroupId) ||
                                   (assetGroupType != "AIGC" && assetGroupType != "LivenessFace")))
            return invalid("Valid asset_group_id and asset_group_type are required together");
        // 第二阶段确定唯一的视频来源，防止同时提交本地、旧任务和资产。
        // edit、extend、reference 只能选择其中一种视频来源。
        // 多张人物参考图是附加输入，不算第二个视频源。
        const int sourceCount = static_cast<int>(!videoPath.empty()) +
                                static_cast<int>(!videoTaskId.empty()) +
                                static_cast<int>(!videoAssetInput.empty());
        if ((mode == "create" && sourceCount != 0) || (mode != "create" && sourceCount != 1))
            return invalid("provide one video source for edit, extend, or reference only");
        if (!videoAssetInput.empty() && videoAssetId.empty())
            return invalid("video_asset_id must be a valid platform asset ID");
        if (!imagePath.empty() && !firstFrameAssetInput.empty())
            return invalid("choose either image_path or first_frame_asset_id");
        if (!lastFramePath.empty() && !lastFrameAssetInput.empty())
            return invalid("choose either last_frame_path or last_frame_asset_id");
        if (!firstFrameAssetInput.empty() && firstFrameAssetId.empty())
            return invalid("first_frame_asset_id must be a valid platform asset ID");
        if (!lastFrameAssetInput.empty() && lastFrameAssetId.empty())
            return invalid("last_frame_asset_id must be a valid platform asset ID");
        if (!firstFrameAssetInput.empty() && mode != "create")
            return invalid("first_frame_asset_id requires create mode");
        if ((!lastFramePath.empty() || !lastFrameAssetInput.empty()) &&
            (mode != "create" || (imagePath.empty() && firstFrameAssetInput.empty())))
            return invalid("last frame requires create mode and a first frame");
        if (!avatarAssetInput.empty() && !authorizedAssetInput.empty())
            return invalid("choose either a virtual avatar or an authorized portrait asset");
        if (!selectedAssetInput.empty() && !referenceAssetIds.empty())
            return invalid("use either a single portrait asset or reference_asset_ids");
        if (!selectedAssetInput.empty()) {
            if (!imagePath.empty() || !lastFramePath.empty() || !firstFrameAssetInput.empty() ||
                !lastFrameAssetInput.empty())
                return invalid("portrait asset cannot be mixed with strict first or last frames");
            if (selectedAssetId.empty())
                return invalid("portrait asset must be a valid platform asset ID");
        }
        const std::size_t referenceCount = referenceImagePaths.size() + referenceAssetIds.size() +
                                           static_cast<std::size_t>(!selectedAssetId.empty());
        // 所有参考图入口合计上限；单张便利字段也要计数，不能绕过九图限制。
        if (referenceCount > static_cast<std::size_t>(model->maximumReferenceImages))
            return invalid(std::string(model->label) + " accepts at most " +
                           std::to_string(model->maximumReferenceImages) + " reference images");
        if (referenceCount != 0 && (!imagePath.empty() || !lastFramePath.empty() ||
                                    !firstFrameAssetInput.empty() || !lastFrameAssetInput.empty()))
            return invalid("reference images cannot be mixed with strict first/last frames");
        for (const Json& candidate : referenceImagePaths) {
            if (!candidate.is_string() || candidate.get<std::string>().empty())
                return invalid("local_image_paths entries must be nonempty strings");
            if (candidate.get<std::string>().rfind("asset://", 0) == 0)
                return invalid("pass asset:// references in reference_asset_ids");
        }
        for (const Json& candidate : referenceAssetIds) {
            if (!candidate.is_string() || arkAssetId(candidate.get<std::string>()).empty())
                return invalid("reference_asset_ids entries must be valid asset IDs");
        }
        // 第三阶段核对付费规格：时长、画幅和分辨率需由调用方明确确认。
        // 不能静默套默认值后把昂贵任务提交出去。
        const Json production = args.value("production", Json::object());
        if (!production.is_object()) return invalid("production must be an object");
        if ((mode == "create" || mode == "reference") &&
            (!production.contains("duration") || !production.contains("ratio") ||
             !production.contains("resolution")))
            return invalid("confirm duration, ratio, and resolution before video generation");
        if ((production.contains("duration") && !production["duration"].is_number_integer()) ||
            (production.contains("ratio") && !production["ratio"].is_string()) ||
            (production.contains("resolution") && !production["resolution"].is_string()) ||
            (production.contains("generate_audio") && !production["generate_audio"].is_boolean()))
            return invalid("production contains an invalid value");
        const int duration =
            production.value("duration", mode == "edit" || mode == "extend" ? -1 : 5);
        const std::string ratio = production.value("ratio", std::string("adaptive"));
        const std::string resolution = production.value("resolution", std::string("720p"));
        if (duration != -1 && (duration < 4 || duration > model->maximumDuration))
            return invalid(std::string(model->label) + " duration must be -1 or 4 to " +
                           std::to_string(model->maximumDuration) + " seconds");
        if (ratio != "adaptive" && ratio != "16:9" && ratio != "9:16" && ratio != "1:1" &&
            ratio != "4:3" && ratio != "3:4" && ratio != "21:9")
            return invalid("unsupported video ratio");
        if (resolution != "480p" && resolution != "720p" &&
            !(model->supports1080p && resolution == "1080p") &&
            !(model->supports4k && resolution == "4k"))
            return invalid(std::string(model->label) +
                           (model->supports4k      ? " resolution must be 480p, 720p, 1080p, or 4k"
                            : model->supports1080p ? " resolution must be 480p, 720p, or 1080p"
                                                   : " resolution must be 480p or 720p"));
        if (model->usesOmniTaskType &&
            (mode == "edit" || mode == "extend" ||
             (mode == "create" && (!imagePath.empty() || !firstFrameAssetId.empty()))) &&
            ratio != "adaptive")
            return invalid(
                "Seedance 2.5 requires ratio=adaptive for edit, extend, or first frames");
        if (model->usesOmniTaskType && mode == "edit" && duration != -1)
            return invalid("Seedance 2.5 edit requires duration=-1");
        std::string instruction = message;
        if (!extra.empty()) instruction += "\nRelevant context: " + extra;
        if (mode == "edit")
            instruction =
                "Edit reference video 1; keep unmentioned content unchanged. " + instruction;
        if (mode == "extend")
            instruction =
                "Extend reference video 1; preserve its subject and style. " + instruction;
        if (mode == "reference")
            instruction = "Create a video inspired by reference video 1. " + instruction;
        if (!avatarAssetInput.empty())
            instruction =
                "Reference image 1 is a platform virtual actor. Preserve its "
                "appearance; do not use the asset ID as a character name. " +
                instruction;
        if (!authorizedAssetInput.empty())
            instruction =
                "Reference image 1 is an authorized real-person portrait. Preserve this "
                "person's identity; do not use the asset ID as a character name. " +
                instruction;
        if (instruction.size() > 8000) return invalid("video instruction is too long");
        // 付费确认后仍以只读 GetAsset 复核 Active 与类型；没有任何素材写入。
        if (hasGroupConstraint) {
            if (auto error = checkAssetGroup(assetGroupId, assetGroupType, context)) return *error;
        }
        if (!selectedAssetId.empty()) {
            const std::string groupType = avatarAssetInput.empty() ? "LivenessFace" : "AIGC";
            if (hasGroupConstraint && assetGroupType != groupType)
                return invalid("asset_group_type conflicts with portrait asset type");
            if (auto error = checkActiveAsset(selectedAssetId, "Image", context, groupType,
                                              hasGroupConstraint ? assetGroupId : std::string{}))
                return *error;
        }
        for (const Json& candidate : referenceAssetIds) {
            if (auto error =
                    checkActiveAsset(arkAssetId(candidate.get<std::string>()), "Image", context, {},
                                     hasGroupConstraint ? assetGroupId : std::string{}))
                return *error;
        }
        if (!firstFrameAssetId.empty()) {
            if (auto error = checkActiveAsset(firstFrameAssetId, "Image", context, {},
                                              hasGroupConstraint ? assetGroupId : std::string{}))
                return *error;
        }
        if (!lastFrameAssetId.empty()) {
            if (auto error = checkActiveAsset(lastFrameAssetId, "Image", context, {},
                                              hasGroupConstraint ? assetGroupId : std::string{}))
                return *error;
        }
        if (!videoAssetId.empty()) {
            if (auto error = checkActiveAsset(videoAssetId, "Video", context, {},
                                              hasGroupConstraint ? assetGroupId : std::string{}))
                return *error;
        }
        if (context.isCanceled())
            return fail(MaiErrorCode::Canceled, "canceled", "Video submission was canceled");
        // 第四阶段生成供应商 content[]：文本、图片和视频保持各自的协议 role。
        // 图片和视频资产分别放入 image_url/video_url 的 asset:// 输入。
        // 编辑任务会把这些参考图与源视频一起放进 content，不再把资产当成本地文件。
        Json content = Json::array({Json{{"type", "text"}, {"text", instruction}}});
        if (!selectedAssetId.empty())
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url", {{"url", "asset://" + selectedAssetId}}},
                                   {"role", "reference_image"}});
        for (const Json& candidate : referenceAssetIds)
            content.push_back(Json{
                {"type", "image_url"},
                {"image_url", {{"url", "asset://" + arkAssetId(candidate.get<std::string>())}}},
                {"role", "reference_image"}});
        if (!firstFrameAssetId.empty())
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url", {{"url", "asset://" + firstFrameAssetId}}},
                                   {"role", "first_frame"}});
        if (!lastFrameAssetId.empty())
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url", {{"url", "asset://" + lastFrameAssetId}}},
                                   {"role", "last_frame"}});
        std::string reference = videoAssetId.empty() ? std::string{} : "asset://" + videoAssetId;
        if (!videoTaskId.empty()) {
            if (!validTaskId(videoTaskId)) return invalid("invalid video_task_id");
            ArkResponse task = requestJson(std::string(kVideoTasksUrl) + "/" + videoTaskId, key,
                                           mCaBundle, nullptr, context);
            if (task.error) return *task.error;
            if (stringValue(task.data, "status") != "succeeded")
                return invalid("video_task_id is not a completed task");
            reference = stringValue(task.data.value("content", Json::object()), "video_url");
        }
        if (!reference.empty()) {
            if (!videoTaskId.empty() && !isHttpsUrl(reference))
                return fail(MaiErrorCode::Protocol, "protocol",
                            "Completed Seedance task returned no HTTPS video URL");
            content.push_back(Json{{"type", "video_url"},
                                   {"video_url", {{"url", reference}}},
                                   {"role", "reference_video"}});
        } else if (mode != "create") {
            return fail(MaiErrorCode::Protocol, "protocol", "No usable source video was prepared");
        }
        Json body = {{"model", modelId},
                     {"content", content},
                     {"duration", duration},
                     {"ratio", ratio},
                     {"resolution", resolution},
                     {"generate_audio", production.value("generate_audio", true)}};
        if (model->usesOmniTaskType) {
            if (mode == "edit" || mode == "extend" || mode == "reference")
                body["omni_reference_task_type"] = mode;
            else if (referenceCount != 0)
                body["omni_reference_task_type"] = "reference";
        }
        if (body.dump().size() > 64'000'000)
            return invalid("Seedance request exceeds the 64 MB body limit");
        // 第五阶段才是收费提交：前面的任何错误都必须在这个边界前返回。
        ArkResponse result = requestJson(kVideoTasksUrl, key, mCaBundle, &body, context);
        if (result.error) return *result.error;
        const std::string taskId = stringValue(result.data, "id");
        // 得到任务 ID 仅代表已受理；后台还要查状态、下载、验证后才能宣称完成。
        if (!validTaskId(taskId))
            return fail(MaiErrorCode::Protocol, "protocol", "Ark returned no valid task ID");
        Json output = {{"task_id", taskId},
                       {"conversation_id", taskId},
                       {"status", stringValue(result.data, "status")},
                       {"bound_model", modelId},
                       {"reply", "The video task has started. The app will report its result."}};
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = stringValue(args, "parent_task_id");
            task.providerTaskId = taskId;
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = !videoTaskId.empty()       ? videoTaskId
                                  : !videoAssetId.empty()    ? videoAssetId
                                  : !selectedAssetId.empty() ? selectedAssetId
                                  : !referenceAssetIds.empty()
                                      ? arkAssetId(referenceAssetIds.front().get<std::string>())
                                  : !firstFrameAssetId.empty() ? firstFrameAssetId
                                                               : std::string{};
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                output["context_persistence_warning"] = stored.message();
                output["reply"] =
                    "The paid video task was submitted, but revision context "
                    "could not be saved. Do not submit it again; continue with "
                    "the returned provider task ID.";
            } else {
                output["conversation_id"] = task.id;
                output["specialist_task_id"] = task.id;
                if (!task.parentTaskId.empty()) output["revision_of"] = task.parentTaskId;
            }
        }
        return MaiToolResult::success(output.dump());
    }

    // 根据原任务标识查询 Seedance 的生成状态，并在完成后下载视频产物。
    // 查询失败保留云任务和本地记录，不能误报为生成失败或触发重新付费。
    MaiToolResult continueTask(const Json& args, const std::string& key,
                               const MaiToolContext& context) const {
        const std::string conversationId = stringValue(args, "conversation_id");
        if (!validTaskId(conversationId))
            return invalid("conversation_id is required and must be valid");
        std::string taskId = conversationId;
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return fail(MaiErrorCode::NotFound, "not_found",
                            "Seedance task was not found in this AI conversation");
            taskId = previous.providerTaskId;
        }
        ArkResponse task;
        std::string status;
        const int attempts = args.value("poll_once", false) ? 1 : 10;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            task = requestJson(std::string(kVideoTasksUrl) + "/" + taskId, key, mCaBundle, nullptr,
                               context);
            if (task.error) return *task.error;
            status = stringValue(task.data, "status");
            if (status.empty())
                return fail(MaiErrorCode::Protocol, "protocol", "Ark returned no task status");
            if (status != "queued" && status != "running") break;
            if (attempt + 1 < attempts) {
                for (int tick = 0; tick < 50; ++tick) {
                    if (context.isCanceled())
                        return fail(MaiErrorCode::Canceled, "canceled", "Task was canceled");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        const std::string boundModel = stringValue(task.data, "model");
        if (status == "cancelled")
            return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                               {"task_id", taskId},
                                               {"status", status},
                                               {"reply", "The cloud video task was canceled."}}
                                              .dump());
        if (status == "failed" || status == "expired") {
            const Json detail = task.data.value("error", Json::object());
            const std::string message = stringValue(detail, "message");
            return fail(MaiErrorCode::Network, "task_failed",
                        message.empty() ? "Seedance task " + status : message);
        }
        if (status != "succeeded")
            return MaiToolResult::success(
                Json{{"conversation_id", conversationId},
                     {"task_id", taskId},
                     {"bound_model", boundModel},
                     {"status", status},
                     {"reply", "The video task is still " + status + "."},
                     {"notice", "continue checks status only; use revise for new feedback"}}
                    .dump());
        const std::string url =
            stringValue(task.data.value("content", Json::object()), "video_url");
        std::string relative;
        if (auto error = outputName({}, "seedance-" + taskId + ".mp4", "mp4", context, relative))
            return *error;
        const std::string path = maiResolvePathWithinRoot(context.root, relative);
        if (MaiFileSystem::exists(MaiFilePath::fromUtf8(path))) {
            std::uint64_t size = 0;
            if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) || size == 0)
                return fail(MaiErrorCode::Internal, "io_error", "Existing video is empty");
            return MaiToolResult::success(
                Json{{"task_id", taskId},
                     {"conversation_id", conversationId},
                     {"bound_model", boundModel},
                     {"status", status},
                     {"path", path},
                     {"bytes", size},
                     {"mime_type", "video/mp4"},
                     {"reply", "The video is complete and saved locally."}}
                    .dump());
        }
        MaiToolResult result = downloadResult(url, relative, 500, mCaBundle, context, false);
        if (result.hasError()) return result;
        Json output = Json::parse(result.output());
        output.update(Json{{"task_id", taskId},
                           {"conversation_id", conversationId},
                           {"bound_model", boundModel},
                           {"status", status},
                           {"reply", "The video is complete and saved locally."}});
        return MaiToolResult::success(output.dump());
    }

    MaiArkApiKeyProvider mKey;
    std::string mCaBundle;
    MaiArkAssetProvider mAssets;
    MaiVideoMetadataProvider mVideoMetadata;
};

class MaiSeedreamImageTool final : public MaiTool {
public:
    MaiSeedreamImageTool(MaiArkApiKeyProvider provider, std::string caBundle,
                         MaiCreativeMediaUploadProvider uploadMedia)
        : mKey(std::move(provider)),
          mCaBundle(std::move(caBundle)),
          mUploadMedia(std::move(uploadMedia)) {}

    std::string name() const override {
        return "seedream_image";
    }
    bool requiresApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return isPaidGenerationAction(raw);
    }
    bool requiresPaidApproval(const std::string& raw) const override {
        return isBillableSubmissionAction(raw);
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        // configured 只表示取到了 API Key。unverified 表示工具已接线，但当前账号的
        // 每项能力尚未全部完成付费实盘验证，不能把它展示成“已确认可用”。
        const bool configured = mKey && !mKey().empty();
        const auto unverified = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                           : MaiSpecialistCapabilityStatus::NotConfigured;
        const auto mediaStatus = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                 : mUploadMedia
                                     ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                     : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        return MaiSpecialistInfo{
            name(),
            kImageModel,
            configured,
            // text_to_image：仅凭文本描述生成一张新图片。
            {{"text_to_image", true, true, unverified, {}},
             // single_image_edit：接一张已有图片并按提示词编辑，原图片不覆盖。
             {"single_image_edit", true, true, mediaStatus,
              "Local image is uploaded to private OSS before paid generation"},
             // multi_image_edit：接 2–10 张可访问图片，融合后只输出一张新图片。
             {"multi_image_edit", true, true, mediaStatus,
              "Accepts 2 to 10 accessible images and produces one image"},
             // interactive_region_edit：支持描述区域编辑，但区域坐标必须写进指令；
             // 这不是独立的区域坐标参数，工具不会自动猜需要改哪个区域。
             {"interactive_region_edit", true, true, mediaStatus,
              "Region coordinates must be supplied in the instruction"},
             // layer_split：模型相关能力虽存在，但当前工具未接分层文件和元数据输出。
             {"layer_split", true, true, MaiSpecialistCapabilityStatus::NotImplemented,
              "Layer output and metadata are not wired"},
             // image_series_output：模型不提供连续图片序列，工具也没有此类输出入口。
             {"image_series_output", false, false, MaiSpecialistCapabilityStatus::NotImplemented,
              "This model does not provide sequential image series output"}}};
    }
    std::string description() const override {
        // 未配置分支：仍允许主模型调用 discover 查看能力，但不能付费提交任务。
        if (!specialistInfo()->configured)
            return "Seedream image specialist bound to doubao-seedream-5-0-flash-260915. Ark API "
                   "Key is not configured on this device. Use discover for capabilities, or ask "
                   "the user to configure the key before delegating.";
        // discover 查询能力；delegate 开新任务；revise 必须带已有 parent_task_id，
        // 并把本次修改意见放进 message，不能凭空继承别的会话。
        return "A model-backed Seedream image specialist. Use discover for capabilities, delegate "
               "a new goal, or revise with parent_task_id set to a previous specialist_task_id "
               "and new feedback. "
               // 不传图是文生图；image_path 是单图编辑；image_paths 为 2–10 图融合。
               // 三条路径都生成新的 PNG，输入原图不改动。
               "It can create from text, edit one image_path, or combine 2 to "
               "10 image_paths into one new PNG. "
               // 主模型描述“要改什么、希望得到什么”；具体模型请求由工具负责构造。
               // 成品须由 agent_send_media 交付给用户，不能只回复一个文件名。
               "Describe edits and desired output in the message, leaving creative details to "
               "Seedream; this specialist handles model "
               "request details. Use agent_send_media to deliver the result.";
    }
    std::string parametersSchema() const override {
        // 顶层是对象；只允许下面列出的字段，不能把任意模型私有参数塞进来。
        return R"({"type":"object","properties":{)"
               // action=discover 只读；delegate/revise 会实际生成图片并经过付费确认。
               R"("action":{"type":"string","enum":["discover","delegate","revise"]},)"
               // message 是本次创作指令；context 是与本次任务相关的背景约束。
               R"("message":{"type":"string"},"context":{"type":"string"},)"
               // image_path 单图；image_paths 数组多图，最少 2 张、最多 10 张。
               R"("image_path":{"type":"string"},"image_paths":{"type":"array",)"
               R"("items":{"type":"string"},"minItems":2,"maxItems":10},)"
               // parent_task_id 只用于修订旧任务；output_path 是工作区的新产物路径。
               R"("parent_task_id":{"type":"string"},"output_path":{"type":"string"},)"
               // 输出档位只接受 1K、1.5K、2K；action 必填，其余字段按动作校验。
               R"("size":{"type":"string",)"
               R"("enum":["1K","1.5K","2K"]}},"required":["action"],)"
               R"("additionalProperties":false})";
    }
    // 解析模型提供的参数并提交或查询方舟媒体任务。
    // 成功和失败都转成明确的工具结果；中间状态不能冒充最终产物。
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        // schema 是模型可见的参数外形，执行层仍要核对字段类型、可访问路径、
        // 修订任务归属和输出新文件，全部通过后才允许调用付费接口。
        Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object()) return invalid("arguments must be an object");
        for (const char* field : {"action", "message", "context", "image_path", "parent_task_id",
                                  "output_path", "size"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        if (args.contains("image_paths") && !args["image_paths"].is_array())
            return invalid("image_paths must be an array of 2 to 10 paths");
        const std::string action = stringValue(args, "action");
        const std::string message = stringValue(args, "message");
        if (message.empty() && (action == "delegate" || action == "revise"))
            return invalid("message is required to start or revise a task");
        if (action == "discover") {
            // Seedream 发现能力不扣费：说明文生图、单图和多图融合，明确分层输出未接线。
            return discoverSpecialist(*specialistInfo(),
                                      "I can create a PNG from text, edit one accessible image, "
                                      "or combine multiple accessible reference images into one. "
                                      "Layer splitting is not wired yet.");
        }
        const std::string key = mKey ? mKey() : std::string{};
        if (action != "delegate" && action != "revise")
            return invalid("action must be discover, delegate, or revise");
        if (key.empty())
            return fail(MaiErrorCode::NotConfigured, "not_configured",
                        "Configure an Ark API key in this platform's Agent settings");
        if (action == "revise") {
            if (context.specialistTasks == nullptr)
                return fail(MaiErrorCode::NotConfigured, "not_configured",
                            "Specialist task history is unavailable");
            MaiSpecialistTask previous;
            const std::string previousId = stringValue(args, "parent_task_id");
            if (!context.specialistTasks->getSpecialistTask(previousId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name() || previous.outputPath.empty())
                return fail(MaiErrorCode::NotFound, "not_found",
                            "Previous Seedream image was not found in this AI conversation");
            if (!stringValue(args, "image_path").empty() || args.contains("image_paths"))
                return invalid("revise uses the previous output; do not override its source");
            args["image_path"] = previous.outputPath;
            std::string summary = previous.contextSummary;
            if (!summary.empty()) summary += "\n";
            summary += "Previous goal: " + previous.intent;
            const std::string current = stringValue(args, "context");
            if (!current.empty()) summary += "\nCurrent constraints: " + current;
            if (summary.size() > 3000)
                return invalid("Revision context is too long; summarize the prior goal");
            args["context"] = std::move(summary);
        }
        std::string prompt = message;
        const std::string extra = stringValue(args, "context");
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (prompt.size() > 4000) return invalid("image task text is too long");
        const std::string size =
            stringValue(args, "size").empty() ? "2K" : stringValue(args, "size");
        if (size != "1K" && size != "1.5K" && size != "2K")
            return invalid("size must be 1K, 1.5K, or 2K");
        Json body = {{"model", kImageModel},   {"prompt", prompt},         {"size", size},
                     {"output_format", "png"}, {"response_format", "url"}, {"watermark", true}};
        const std::string imagePath = stringValue(args, "image_path");
        const Json imagePaths = args.value("image_paths", Json{});
        if (!imagePath.empty() && !imagePaths.is_null())
            return invalid("provide image_path or image_paths, not both");
        if (!imagePaths.is_null()) {
            if (!imagePaths.is_array() || imagePaths.size() < 2 || imagePaths.size() > 10)
                return invalid("image_paths must contain 2 to 10 paths");
            Json images = Json::array();
            for (const Json& candidate : imagePaths) {
                if (!candidate.is_string() || candidate.get<std::string>().empty())
                    return invalid("each image_paths entry must be a nonempty path");
                Json image;
                if (auto error =
                        readImage(candidate.get<std::string>(), context, mUploadMedia, image))
                    return *error;
                images.push_back(std::move(image));
            }
            body["image"] = std::move(images);
        }
        if (!imagePath.empty()) {
            Json image;
            if (auto error = readImage(imagePath, context, mUploadMedia, image)) return *error;
            body["image"] = std::move(image);
        }
        std::string relative;
        if (auto error = outputName(stringValue(args, "output_path"),
                                    "seedream-" + MaiIdGenerator::generate("file_") + ".png", "png",
                                    context, relative))
            return *error;
        if (MaiFileSystem::exists(
                MaiFilePath::fromUtf8(maiResolvePathWithinRoot(context.root, relative))))
            return invalid("output_path already exists");
        ArkResponse response = requestJson(kImagesUrl, key, mCaBundle, &body, context);
        if (response.error) return *response.error;
        if (!response.data.contains("data") || !response.data["data"].is_array() ||
            response.data["data"].empty())
            return fail(MaiErrorCode::Protocol, "protocol", "Ark returned no image");
        const std::string url = stringValue(response.data["data"][0], "url");
        MaiToolResult result = downloadResult(url, relative, 50, mCaBundle, context, true);
        if (result.hasError()) return result;
        Json output = Json::parse(result.output());
        output["model"] = kImageModel;
        output["size"] = size;
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = stringValue(args, "parent_task_id");
            task.intent = message;
            task.contextSummary = extra;
            task.inputReference = imagePath.empty() ? "multiple_images" : imagePath;
            task.outputPath = stringValue(output, "path");
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                output["context_persistence_warning"] = stored.message();
                output["revision_hint"] =
                    "The image was generated; do not repeat this paid "
                    "request. Use the returned path for any further edit.";
            } else {
                output["specialist_task_id"] = task.id;
                if (!task.parentTaskId.empty()) output["revision_of"] = task.parentTaskId;
            }
        }
        return MaiToolResult::success(output.dump());
    }

private:
    MaiArkApiKeyProvider mKey;
    std::string mCaBundle;
    MaiCreativeMediaUploadProvider mUploadMedia;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiSeedanceVideoTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath,
                                                  MaiArkAssetProvider assets,
                                                  MaiVideoMetadataProvider videoMetadata) {
    return std::make_unique<MaiSeedanceVideoTool>(std::move(apiKey), std::move(caBundlePath),
                                                  std::move(assets), std::move(videoMetadata));
}

std::unique_ptr<MaiTool> makeMaiSeedreamImageTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath,
                                                  MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiSeedreamImageTool>(std::move(apiKey), std::move(caBundlePath),
                                                  std::move(uploadMedia));
}
