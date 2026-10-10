#include "MaiArkMediaTools.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "MaiBlockingCheck.h"
#include "MaiDownloadFileTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiPathGuard.h"

namespace {

using Json = nlohmann::json;

bool isPaidGenerationAction(const std::string& raw) {
    // cancel/delete 不新建付费任务，但会取消或删除已有任务，仍需逐次确认。
    const Json args = Json::parse(raw, nullptr, false);
    if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
    const std::string action = args["action"].get<std::string>();
    return action != "discover" && action != "continue";
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
                         MaiCreativeMediaUploadProvider uploadMedia)
        : mKey(std::move(provider)),
          mCaBundle(std::move(caBundle)),
          mUploadMedia(std::move(uploadMedia)) {}

    std::string name() const override {
        return "seedance_video";
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (args.is_object() && stringValue(args, "model") == kVideoModel) return false;
        if (args.is_object() && stringValue(args, "action") == "delegate") {
            if (!mUploadMedia && !stringValue(args, "video_path").empty()) return false;
            const std::string mode = stringValue(args, "mode");
            if (mode == "reference" || mode == "edit" || mode == "extend") {
                const int sourceCount =
                    static_cast<int>(!stringValue(args, "video_path").empty()) +
                    static_cast<int>(!stringValue(args, "video_url").empty()) +
                    static_cast<int>(!stringValue(args, "video_task_id").empty());
                if (sourceCount != 1) return false;
            }
        }
        return isPaidGenerationAction(raw);
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        // discover 按实际配置报告能力：Mini 为默认型号，Fast/标准版可选；2.5 因费用禁用。
        // 本地视频编辑只有上传回调存在时才标记为已接线，实盘状态仍如实标为待验证。
        const bool configured = mKey && !mKey().empty();
        const auto unverified = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                           : MaiSpecialistCapabilityStatus::NotConfigured;
        const auto localVideoStatus = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                      : mUploadMedia
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
              "Seedance 2.0 Mini accepts a first frame; confirm the output settings before paying"},
             // video_edit_from_url：用远端视频 URL 作为编辑源。
             {"video_edit_from_url", true, true, unverified,
              "mode=edit accepts exactly one accessible HTTPS video_url; image references may "
              "be added, but strict first/last frames cannot be mixed in"},
             // video_edit_from_seedance_task：用已完成的 Seedance 任务作为编辑源。
             {"video_edit_from_seedance_task", true, true, unverified,
              "mode=edit accepts one completed Seedance video_task_id from the same provider; "
              "the referenced task must be queryable"},
             // video_extend_from_url：对远端视频 URL 对应内容做延长。
             {"video_extend_from_url", true, true, unverified,
              "mode=extend accepts one accessible video_url, one local video_path uploaded to "
              "private OSS, or one completed video_task_id"},
             // video_reference_from_url：将远端视频 URL 作为参考视频而非严格首帧。
             {"video_reference_from_url", true, true, unverified,
              "mode=reference requires exactly one video source; video_path, video_url, and "
              "video_task_id are mutually exclusive"},
             // first_and_last_frame_to_video：必须同时给 image_path 和 last_frame_path。
             {"first_and_last_frame_to_video", true, true, unverified,
              "mode=create requires exactly one image_path followed by one last_frame_path; "
              "strict frames cannot be combined with arbitrary reference images"},
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
             // local_portrait_asset_registration：配置了签发服务后，本地 JPEG/PNG 先直传
             // 私有 OSS，再登记为 Ark AIGC 素材，等 Active 才可用。真人脸可能被拒，
             // 或须另走 begin_real_validation 的 H5 本人认证。
             {"local_portrait_asset_registration", true, true, unverified,
              "ark_assets can upload a local JPEG/PNG directly to private OSS when its signer "
              "is configured, then register it as an Ark AIGC asset. Confirm Active before use. "
              "Ark may reject a real face or require separate H5 authorization; "
              "begin_real_validation starts that H5 flow"},
             // multi_reference_video：1–9 张图可来自本地图或 Active 资产 ID；
             // 参考模式不能混严格首尾帧，视频参考最多一个。
             {"multi_reference_video", true, true, unverified,
              "Up to nine reference images total through local_image_paths or Active "
              "reference_asset_ids. Local non-person images are uploaded to private OSS; "
              "registered assets use asset IDs. These references cannot mix with strict "
              "first/last-frame control. At most one video source is wired"},
             // video_edit_from_local_file：有上传回调时先传私有 OSS，交给 Ark 短期 HTTPS
             // 读取地址；云端能否抓取尚待实测。没配置回调时如实标记上传未配置。
             {"video_edit_from_local_file", true, true, localVideoStatus,
              mUploadMedia
                  ? "The host uploads a local video to private object storage and hands Ark a "
                    "temporary HTTPS read URL. Check the source is 2-15 seconds and 24-60 fps "
                    "before paid submission; live Ark fetch remains unverified"
                  : "Local video upload is not configured"},
             // 多段视频参考只在供应商文档里出现，当前适配器没有对应输入数组。
             {"multiple_reference_videos", false, false,
              MaiSpecialistCapabilityStatus::NotImplemented,
              "This tool accepts exactly one video source, not a list of videos"},
             // 管理动作和生成动作的条件不同，运行中的任务不能当作可取消。
             {"task_cancel_delete", false, true, unverified,
              "cancel applies only to queued tasks; delete removes delivered succeeded, "
              "failed, or expired Ark records. Running tasks cannot be canceled or deleted"}}};
    }
    std::string description() const override {
        // 未配置分支：默认型号是 Seedance 2.0 Mini；没有方舟 API Key 时只能查询能力，
        // 不能把“已注册工具”误当成“可以提交生成任务”。
        if (!specialistInfo()->configured)
            return "Seedance video specialist defaults to doubao-seedance-2-0-mini-260615. Ark "
                   "API Key "
                   "is not configured on this device. Use discover for capabilities, or ask the "
                   "user to configure the key before delegating.";
        // 以下英文是给主模型的完整调用说明，中文按原描述的顺序对应：
        // 1. model 可选 Mini（默认）、Fast、标准版 2.0；2.5 因费用已禁用。
        //    每次付费前向用户确认型号，并展示该型号真实的时长和分辨率。
        // 2. discover 查能力；delegate 新建；revise 带旧 conversation_id 和反馈修订，
        //    修订只能沿用同一 AI 会话的原目标和源视频。
        // 3. 已接文生、首帧、首尾帧、参考视频路径，但共享 C++ 路径仍需云端实盘验证。
        //    create/reference 付费前必须确认 duration、ratio、resolution。
        // 4. 标准版支持 4–15 秒及 480p/720p/1080p/4k；Fast/Mini 支持 4–15 秒及
        //    480p/720p；-1 由平台自动决定时长。配置了上传通道时，video_path 会先
        //    上传为临时 HTTPS 地址；付费前用 ffprobe 检查源视频 2–15 秒、24–60 FPS。
        // 5. 普通虚拟素材先在 ark_assets 登记并等 Active，单张用
        //    virtual_avatar_asset_id，多张用 reference_asset_ids。模型看到的是
        //    asset://<ID> 形式的参考图；提示词用“参考图 1”等位置称呼人物。
        // 6. 已授权真人素材用 authorized_portrait_asset_id；需要本人验证时，
        //    begin_real_validation 产生 H5 链接，完成后 get_real_validation 取组 ID。
        //    不能把 AIGC 入库当作真人授权，也不能承诺平台一定接受每张脸。
        // 7. 没有源视频时 create 可用本地图；有源视频时 reference/edit 只接一个
        //    视频源。编辑人物外观可同时传参考素材。local_image_paths 是本地图
        //    上传入口，工具先上传 OSS 再给模型短期 HTTPS 地址；reference_asset_ids 是
        //    已登记素材，总数最多九张；不得与严格首尾帧混用。
        // 8. App 自动跟进完成结果；continue 仅用于手动诊断。成品用
        //    agent_send_media 交付给用户。cancel 只能停止排队任务；delete 只能
        //    删除已结束且已交付的云端记录；两者都不能停止运行中的任务。
        return "Seedance video through Ark. Modes: create from text or strict first/last "
               "frames or up to nine reference images; reference/edit/extend use exactly one "
               "source video from video_path, video_url, or video_task_id. model selects "
               "Seedance 2.0 Mini "
               "(default), 2.0 Fast, or standard 2.0. Seedance 2.5 is disabled because of "
               "its cost. Ask the user which tier to use before a paid call and "
               "show its actual duration and resolution. Use discover for capabilities, delegate "
               "to start a task, or revise with a previous "
               "conversation_id and new feedback. Revision keeps the previous goal and source "
               "video in the same AI session. Text, first-frame, "
               "first-and-last-frame, and reference-video paths are implemented; cloud validation "
               "is still needed for the shared C++ path. Confirm duration, ratio and resolution "
               "with the user before paid create or reference calls. Standard 2.0 supports "
               "4-15 seconds and 480p/720p/1080p/4k; 2.0 Fast and Mini support 4-15 seconds "
               "and 480p/720p. All three accept -1 for automatic duration. Local video_path is "
               "uploaded "
               "by the host media service when configured; discover reports its availability. "
               "Before a paid local-video task, use ffprobe to check input duration: 2-15 "
               "seconds; FPS 24-60. "
               "For a private virtual avatar, use ark_assets to submit an Ark-accessible HTTPS "
               "image and wait for Active, or select an existing asset in the Ark Experience "
               "Center. Pass its virtual_avatar_asset_id. The tool sends asset://<ID> as "
               "reference image 1; refer to it as image 1 in the message. For several Active "
               "assets, pass all IDs in reference_asset_ids and do not resend local paths. "
               "Ark decides whether "
               "the asset is acceptable. For a separately authorized real-person portrait, "
               "pass authorized_portrait_asset_id only after Ark marks it authorized in this "
               "account. ark_assets begin_real_validation generates an H5 link when the user "
               "chooses real-person verification; get_real_validation returns the group ID "
               "after that person completes it. If the original references clearly depict real "
               "people, check or "
               "register suitable assets before a paid call. If Ark rejects an otherwise "
               "uncertain original photo for a possible real face, use ark_assets to register "
               "the unchanged image and wait for "
               "Active before retrying the same model with its asset ID. Registration can also "
               "fail or require a separate authorization path; follow the actual provider "
               "result. Use ark_assets list_groups/list_assets with group_type=LivenessFace "
               "for real-portrait assets owned by this Ark account; other accounts' authorized "
               "assets may not be listed. "
               "For non-real-person photos without a source video, use mode=create with "
               "local_image_paths. The tool uploads each local image to private OSS and "
               "passes a temporary HTTPS URL to Ark, never Base64. For real-person photos, "
               "register all selected images as Active assets first and pass their IDs in "
               "reference_asset_ids. mode=reference requires one existing video source. "
               "To edit an uploaded video with a person's appearance, use mode=edit, exactly "
               "one video source, and reference_asset_ids or local_image_paths. A local "
               "video_path is uploaded to private OSS before Ark receives a reference_video URL. "
               "Use local_image_paths for local files and reference_asset_ids for multiple "
               "Active asset:// images, up to 9 total; Ark validates each asset. Reference images "
               "cannot "
               "be mixed with strict first/last-frame control. The app checks tasks and hands "
               "completed results back automatically; "
               "continue is for manual diagnostics only. Cancel can stop only a queued task. "
               "Delete removes a delivered succeeded, failed, or expired Ark task record; "
               "neither operation can stop a running task. Downloaded media remains local. "
               "Use agent_send_media to deliver a "
               "completed video.";
    }
    std::string parametersSchema() const override {
        // JSON Schema 只定义字段形状；execute 还会核对模式组合、文件存在、数量、
        // 付费规格与供应商能力。不能因为 schema 接受字段就假定某个组合可用。
        const std::string raw =
            R"({"type":"object","properties":{)"
            // action 为能力查询、提交、跟进、云端取消、云端删除或修订；
            // cancel/delete 要求当前会话的 conversation_id，不能传任意云端 ID。
            R"("action":{"type":"string","enum":["discover","delegate","continue","cancel","delete","revise"]},)"
            // model 是 2.0 Mini/Fast/标准版三选一；2.5 故意不列入可提交值。
            R"("model":{"type":"string","enum":["doubao-seedance-2-0-mini-260615",)"
            R"("doubao-seedance-2-0-fast-260128","doubao-seedance-2-0-260128"]},)"
            // message 是视频要求；context 只带相关背景，修订时用 conversation_id。
            R"("message":{"type":"string"},"context":{"type":"string"},)"
            R"("conversation_id":{"type":"string"},"mode":{"type":"string",)"
            // create 生成；edit 改已有视频；extend 续写；reference 参考已有视频。
            R"("enum":["create","edit","extend","reference"]},)"
            // image_path/last_frame_path 是严格首尾帧，不与任意参考图混用。
            R"("image_path":{"type":"string"},"last_frame_path":{"type":"string"},)"
            // 两种单人资产入口互斥；前者是虚拟素材，后者需要平台真人授权。
            R"("virtual_avatar_asset_id":{"type":"string"},)"
            R"("authorized_portrait_asset_id":{"type":"string"},)"
            // local_image_paths 是 OSS 上传源；reference_asset_ids 传 Active
            // 的资产 ID，可一次提交多张；不能把 asset:// 当本地 path 传入。
            R"("local_image_paths":{"type":"array","items":{"type":"string"}},)"
            R"("reference_asset_ids":{"type":"array","items":{"type":"string"},"maxItems":9},)"
            // 一个视频源可来自本地文件、可访问外链或已完成 Seedance 任务，三选一。
            R"("video_path":{"type":"string"},"video_url":{"type":"string"},)"
            R"("video_task_id":{"type":"string"},"poll_once":{"type":"boolean"},)"
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
            "discover is read-only; delegate creates a paid task; continue polls; revise "
            "creates a paid edit from a completed task; cancel/delete require a saved local "
            "conversation_id and provider-supported state";
        // model：默认 Mini；Mini/Fast 只支持 480p/720p，标准版另支持 1080p/4k；
        // 三款 2.0 型号均为 4–15 秒。2.5 因费用禁用，不能写入可选枚举。
        fields["model"]["description"] =
            "2.0 Mini is default. Mini/Fast: 4-15 seconds, 480p/720p. Standard: 4-15 seconds, "
            "480p/720p/1080p/4k. Seedance 2.5 is disabled";
        // mode：create 不带源视频；reference/edit/extend 都必须从本地文件、
        // 外链或旧 Seedance 任务中恰好选一个视频源。
        fields["mode"]["description"] =
            "create has no source video; reference/edit/extend require exactly one source in "
            "video_path, video_url, or video_task_id";
        // message 写视频目标，context 写必要背景，不能把素材字节或全量聊天历史塞进去。
        fields["message"]["description"] = "Video creation or edit instruction for this task";
        fields["context"]["description"] =
            "Only relevant constraints and background; not media bytes or full chat history";
        // image_path 是严格首帧，last_frame_path 是严格尾帧；尾帧必须配首帧，
        // 这条控制路径和任意多图参考不能混用。
        fields["image_path"]["description"] =
            "Local strict first frame, uploaded to private OSS; create mode only";
        fields["last_frame_path"]["description"] =
            "Local strict last frame; requires image_path and cannot mix with reference images";
        // local_image_paths 是待上传 OSS 的普通本地图；reference_asset_ids 是
        // 已在方舟入库且状态 Active 的资产 ID，不能把 asset:// 当成本地文件名。
        // 两组图片合计最多九张；真人素材应按用户选定的资产/授权路径引用。
        fields["local_image_paths"]["description"] =
            "Local non-portrait reference images for private OSS upload; combined with "
            "reference_asset_ids, at most nine total. Use assets for registered portraits";
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
        // 视频源三选一：video_path 是本地 MP4/MOV 上传源，须先用 ffprobe
        // 核对 2–15 秒及 24–60 fps；video_url 是 HTTPS 外链；
        // video_task_id 必须是可读取且已完成的方舟任务。
        fields["video_path"]["description"] =
            "One local MP4/MOV upload source; check 2-15 seconds and 24-60 fps before paying";
        fields["video_url"]["description"] = "One accessible HTTPS source video URL";
        fields["video_task_id"]["description"] =
            "One completed Seedance provider task ID used as source video";
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
            "Query status once; the App normally polls and delivers the result automatically";
        return schema.dump();
    }
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
              "last_frame_path", "virtual_avatar_asset_id", "authorized_portrait_asset_id",
              "video_path", "video_url", "video_task_id"}) {
            if (args.contains(field) && !args[field].is_string())
                return invalid(std::string(field) + " must be a string");
        }
        if (args.contains("local_image_paths") && !args["local_image_paths"].is_array())
            return invalid("local_image_paths must be an array");
        if (args.contains("reference_asset_ids") && !args["reference_asset_ids"].is_array())
            return invalid("reference_asset_ids must be an array");
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
                "Check video_edit_from_local_file before passing video_path; its availability "
                "depends on the host.");
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
            if (!stringValue(args, "video_url").empty() ||
                !stringValue(args, "video_path").empty() ||
                !stringValue(args, "video_task_id").empty())
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
        return invalid("action must be discover, delegate, continue, cancel, delete, or revise");
    }

private:
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

    MaiToolResult delegate(const Json& args, const std::string& key,
                           const MaiToolContext& context) const {
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
        const std::string avatarAssetInput = stringValue(args, "virtual_avatar_asset_id");
        const std::string authorizedAssetInput = stringValue(args, "authorized_portrait_asset_id");
        const std::string selectedAssetInput =
            avatarAssetInput.empty() ? authorizedAssetInput : avatarAssetInput;
        const std::string selectedAssetId =
            selectedAssetInput.empty() ? std::string{} : arkAssetId(selectedAssetInput);
        const Json referenceImagePaths = args.value("local_image_paths", Json::array());
        const Json referenceAssetIds = args.value("reference_asset_ids", Json::array());
        const std::string videoPath = stringValue(args, "video_path");
        const std::string videoUrl = stringValue(args, "video_url");
        const std::string videoTaskId = stringValue(args, "video_task_id");
        // edit、extend、reference 只能选择本地文件、外链、已有任务三者中的一个。
        // 多张人物参考图是附加输入，不算第二个视频源。
        const int sourceCount = static_cast<int>(!videoPath.empty()) +
                                static_cast<int>(!videoUrl.empty()) +
                                static_cast<int>(!videoTaskId.empty());
        if ((mode == "create" && sourceCount != 0) || (mode != "create" && sourceCount != 1))
            return invalid("provide one video source for edit, extend, or reference only");
        if (!lastFramePath.empty() && (mode != "create" || imagePath.empty()))
            return invalid("last_frame_path requires create mode and image_path as first frame");
        if (!avatarAssetInput.empty() && !authorizedAssetInput.empty())
            return invalid("choose either a virtual avatar or an authorized portrait asset");
        if (!selectedAssetInput.empty() && !referenceAssetIds.empty())
            return invalid("use either a single portrait asset or reference_asset_ids");
        if (!selectedAssetInput.empty()) {
            if (!imagePath.empty() || !lastFramePath.empty())
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
        if (referenceCount != 0 && (!imagePath.empty() || !lastFramePath.empty()))
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
        std::string localVideoPath;
        if (!videoPath.empty()) {
            if (!mUploadMedia)
                return fail(MaiErrorCode::NotConfigured, "upload_not_configured",
                            "Local video upload is unavailable; use video_url or video_task_id");
            localVideoPath = context.resolvePath(videoPath);
            if (localVideoPath.empty()) return invalid("video_path is outside the accessible area");
            const std::string extension = MaiFilePath::fromUtf8(localVideoPath).baseName().toUtf8();
            const std::size_t dot = extension.find_last_of('.');
            std::string suffix = dot == std::string::npos ? std::string{} : extension.substr(dot);
            std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (suffix != ".mp4" && suffix != ".mov")
                return invalid("video_path must be an MP4 or MOV file");
            std::uint64_t bytes = 0;
            if (MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(localVideoPath)) ||
                !MaiFileSystem::fileSize(MaiFilePath::fromUtf8(localVideoPath), bytes) ||
                bytes == 0 || bytes > 200'000'000)
                return invalid("video_path must contain 1 to 200000000 bytes");
        }
        // create/reference 付费前要显式确认时长、画幅和分辨率，不能静默用默认值。
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
            (mode == "edit" || mode == "extend" || (mode == "create" && !imagePath.empty())) &&
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
        // 参考资产只能作为 image_url 的 asset:// 输入；本地路径只用于读取原始图像。
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
        for (const Json& candidate : referenceImagePaths) {
            Json referenceImage;
            if (auto error =
                    readImage(candidate.get<std::string>(), context, mUploadMedia, referenceImage))
                return *error;
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url", {{"url", referenceImage}}},
                                   {"role", "reference_image"}});
        }
        if (!imagePath.empty()) {
            Json image;
            if (auto error = readImage(imagePath, context, mUploadMedia, image)) return *error;
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url", {{"url", image}}},
                                   {"role", mode == "create" ? "first_frame" : "reference_image"}});
        }
        if (!lastFramePath.empty()) {
            Json lastFrame;
            if (auto error = readImage(lastFramePath, context, mUploadMedia, lastFrame))
                return *error;
            content.push_back(Json{{"type", "image_url"},
                                   {"image_url", {{"url", lastFrame}}},
                                   {"role", "last_frame"}});
        }
        std::string reference = videoUrl;
        if (!videoTaskId.empty()) {
            if (!validTaskId(videoTaskId)) return invalid("invalid video_task_id");
            ArkResponse task = requestJson(std::string(kVideoTasksUrl) + "/" + videoTaskId, key,
                                           mCaBundle, nullptr, context);
            if (task.error) return *task.error;
            if (stringValue(task.data, "status") != "succeeded")
                return invalid("video_task_id is not a completed task");
            reference = stringValue(task.data.value("content", Json::object()), "video_url");
        }
        if (!localVideoPath.empty()) {
            // 用户上传的视频先直传 OSS，短期读取链接仅在本次 Seedance 请求中使用。
            auto uploaded = mUploadMedia(localVideoPath, context);
            if (!uploaded)
                return fail(uploaded.error().code(), "upload_failed", uploaded.error().message());
            reference = uploaded.value();
        }
        if (!reference.empty()) {
            if (!isHttpsUrl(reference)) return invalid("video_url must be a public HTTPS URL");
            content.push_back(Json{{"type", "video_url"},
                                   {"video_url", {{"url", reference}}},
                                   {"role", "reference_video"}});
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
        // 素材读取、上传与规格校验完成后才提交一次收费请求。
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
                                  : !videoPath.empty()       ? videoPath
                                  : !videoUrl.empty()        ? videoUrl
                                  : !selectedAssetId.empty() ? selectedAssetId
                                  : !referenceAssetIds.empty()
                                      ? arkAssetId(referenceAssetIds.front().get<std::string>())
                                      : imagePath;
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
    MaiCreativeMediaUploadProvider mUploadMedia;
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
                                                  MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiSeedanceVideoTool>(std::move(apiKey), std::move(caBundlePath),
                                                  std::move(uploadMedia));
}

std::unique_ptr<MaiTool> makeMaiSeedreamImageTool(MaiArkApiKeyProvider apiKey,
                                                  std::string caBundlePath,
                                                  MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiSeedreamImageTool>(std::move(apiKey), std::move(caBundlePath),
                                                  std::move(uploadMedia));
}
