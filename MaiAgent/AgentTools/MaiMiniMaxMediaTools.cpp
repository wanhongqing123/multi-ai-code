#include "MaiMiniMaxMediaTools.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MaiCreativeMediaSupport.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiTime.h"

namespace {

using Json = nlohmann::json;
constexpr char kApiBase[] = "https://api.minimax.cn";

std::string value(const Json& data, const char* field) {
    return data.is_object() && data.contains(field) && data[field].is_string()
               ? data[field].get<std::string>()
               : std::string{};
}

std::string describeMiniMaxVideoSchema(const std::string& raw) {
    Json schema = Json::parse(raw);
    Json& fields = schema["properties"];
    // H3 的扁平字段和 content[] 最终合并为一次请求；主模型须先选清控制方式。
    // action：discover 查当前能力；validate 只在本地检查且不上传、不扣费；
    // delegate 才付费生成；continue 查询旧任务；delete 只删本地已交付记录。
    fields["action"]["description"] =
        "discover lists capabilities; validate checks locally without upload or charge; "
        "delegate is paid; continue polls; delete removes only a delivered local record";
    // model：H3 可生成 4–15 秒的 768P/2K，H3 Max 为 5–15 秒的 480P/768P；
    // 两者都没有原生 1080P。duration、resolution 要放在顶层并匹配型号。
    fields["model"]["description"] =
        "MiniMax-H3: 4-15 seconds at 768P/2K; MiniMax-H3-Max: 5-15 seconds at 480P/768P; "
        "neither natively supports 1080P";
    fields["duration"]["description"] = "Required top-level output duration in seconds";
    fields["resolution"]["enum"] = Json::array({"480P", "768P", "2K"});
    fields["resolution"]["description"] =
        "Required top-level resolution matching the selected H3 or H3 Max variant";
    // ratio：纯文生必须显式给具体画幅；首尾帧或多模态参考可用 adaptive。
    fields["ratio"]["enum"] =
        Json::array({"adaptive", "21:9", "16:9", "4:3", "1:1", "3:4", "9:16"});
    fields["ratio"]["description"] =
        "Concrete ratio required for text-only generation; reference/frame modes may use "
        "adaptive. Values: 21:9, 16:9, 4:3, 1:1, 3:4, 9:16, adaptive";
    // message/context 分别是本次目标与相关背景；不能塞入媒体字节或完整聊天历史。
    fields["message"]["description"] = "Required scene or edit instruction for this task";
    fields["context"]["description"] =
        "Only task-relevant constraints; not the entire conversation or media bytes";
    // first_frame 与 image_path 同义，last_frame_path 与 last_frame 同义。
    // 它们是严格首尾帧，不得与多模态参考混合；不是无限多图输入。
    fields["first_frame"]["description"] =
        "One strict opening frame; alias image_path. Cannot mix with multimodal references";
    fields["image_path"]["description"] = "Alias for one strict first_frame image";
    fields["last_frame_path"]["description"] =
        "One strict ending frame; alias last_frame. Cannot mix with multimodal references";
    fields["last_frame"]["description"] = "Alias for one strict last_frame_path image";
    // local_image_paths 是最多九张参考图；图片/视频经私有 OSS，
    // 参考音频仍通过 MiniMax 自己的媒体上传接口，不走这条 OSS 图片路径。
    fields["local_image_paths"]["maxItems"] = 9;
    fields["local_image_paths"]["description"] =
        "Up to nine reference images uploaded to private OSS; not strict first/last frames";
    // reference_video_path、reference_video、video_path 只是单个参考视频别名，
    // 不是可保证保留原视频画面的专用视频编辑模式。更多视频可在 content 中列出。
    fields["reference_video_path"]["description"] =
        "One local reference video, uploaded to private OSS; up to three videos via content";
    fields["reference_video"]["description"] = "Alias for reference_video_path";
    fields["video_path"]["description"] =
        "Alias for one reference video; not a dedicated source-video edit mode";
    fields["reference_audio_path"]["description"] =
        "One local reference audio uploaded by provider media API; up to three via content";
    fields["reference_audio"]["description"] = "Alias for reference_audio_path";
    // content[] 用 type 标记文字/图片/视频/音频，媒体条目还需本地 path 和角色。
    // 图片可用 reference_image 或兼容的 reference；省略多图角色会自动归一化。
    // 最多九图、三视频、三音频；不得和严格首尾帧控制混用。
    fields["content"]["description"] =
        "Multimodal items: text or local media paths with roles. Up to nine reference images, "
        "three videos, three audio clips. Strict frames cannot mix with references";
    Json& itemFields = fields["content"]["items"]["properties"];
    itemFields["type"]["description"] =
        "text, image_url, video_url, or audio_url; media path is a local source file";
    itemFields["text"]["description"] = "Additional text instruction for this item";
    itemFields["role"]["description"] =
        "For media use first_frame, last_frame, reference_image, reference_video, or "
        "reference_audio. Generic reference and omitted image roles are normalized";
    itemFields["path"]["description"] =
        "Local file path, not Base64 or a public URL; images/videos use private OSS";
    // prompt_expansion_mode 只允许 H3 Max 使用 disabled/balanced/quality；
    // conversation_id 用旧任务做查询/删除，parent_task_id 仅建立本地修订链；
    // poll_once 仅查一次，正常结果交付由 App 后台自动完成。
    fields["prompt_expansion_mode"]["enum"] = Json::array({"disabled", "balanced", "quality"});
    fields["prompt_expansion_mode"]["description"] =
        "Only for MiniMax-H3-Max: disabled, balanced, or quality";
    fields["conversation_id"]["description"] =
        "Saved spt_ task ID for continue or local-record delete in this AI conversation";
    fields["parent_task_id"]["description"] =
        "Optional prior local task ID for revision lineage; inputs are not reused automatically";
    fields["poll_once"]["description"] =
        "Query status once; the App normally polls and delivers the result automatically";
    return schema.dump();
}

std::string idValue(const Json& data, const char* field) {
    if (!data.is_object() || !data.contains(field)) return {};
    const Json& id = data[field];
    if (id.is_string()) return id.get<std::string>();
    return id.is_number_integer() ? std::to_string(id.get<std::int64_t>()) : std::string{};
}

enum class MediaKind { Image, Video, Audio };

struct Attachment {
    MediaKind kind;
    std::string role;
    std::string path;
};

struct UploadedReference {
    std::string reference;
    std::optional<MaiToolResult> error;
};

struct LocalReference {
    std::string path;
    std::string extension;
    std::uint64_t size = 0;
};

std::optional<MaiToolResult> inspectReference(const Attachment& attachment,
                                              const MaiToolContext& context,
                                              LocalReference& local) {
    local.path = context.resolvePath(attachment.path);
    if (local.path.empty()) return maiCreativeInvalid("Reference file is not accessible");
    const std::size_t dot = local.path.find_last_of('.');
    if (dot == std::string::npos)
        return maiCreativeInvalid("Reference file needs a supported extension");
    local.extension = local.path.substr(dot);
    std::transform(local.extension.begin(), local.extension.end(), local.extension.begin(),
                   [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    const bool supported = attachment.kind == MediaKind::Image
                               ? local.extension == ".jpg" || local.extension == ".jpeg" ||
                                     local.extension == ".png" || local.extension == ".webp" ||
                                     local.extension == ".heic" || local.extension == ".heif"
                           : attachment.kind == MediaKind::Video
                               ? local.extension == ".mp4" || local.extension == ".mov"
                               : local.extension == ".wav" || local.extension == ".mp3";
    if (!supported) return maiCreativeInvalid("Reference file format is unsupported");
    const std::uint64_t maximum = attachment.kind == MediaKind::Image   ? 30'000'000
                                  : attachment.kind == MediaKind::Video ? 50'000'000
                                                                        : 15'000'000;
    if (!MaiFileSystem::fileSize(MaiFilePath::fromUtf8(local.path), local.size) ||
        local.size == 0 || local.size > maximum)
        return maiCreativeInvalid("Reference file exceeds its provider size limit");
    return std::nullopt;
}

UploadedReference uploadReference(const Attachment& attachment, const std::string& key,
                                  const std::string& caBundle, const MaiToolContext& context) {
    LocalReference local;
    if (auto error = inspectReference(attachment, context, local)) return {{}, *error};
    std::string bytes;
    bool truncated = false;
    if (MaiFileSystem::readFile(MaiFilePath::fromUtf8(local.path), bytes, local.size + 1,
                                &truncated) ||
        truncated)
        return {{}, maiCreativeInvalid("Reference file could not be read")};
    const MaiCreativeHttpResult uploaded =
        maiCreativeUploadFile(std::string(kApiBase) + "/v1/files/upload", key, caBundle,
                              "asset" + local.extension, bytes, "video_generation_input", context);
    if (uploaded.error) return {{}, *uploaded.error};
    const Json data = Json::parse(uploaded.body, nullptr, false);
    const std::string id = idValue(data.value("file", Json::object()), "file_id");
    if (!maiCreativeValidId(id))
        return {{},
                maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                   "MiniMax upload returned no file ID")};
    return {"mm_file://" + id, std::nullopt};
}

std::optional<MaiToolResult> appendAttachment(std::vector<Attachment>& attachments, MediaKind kind,
                                              const std::string& role, const std::string& path) {
    if (path.empty()) return maiCreativeInvalid("Reference path is required");
    const bool validRole = kind == MediaKind::Image
                               ? role.empty() || role == "reference" || role == "first_frame" ||
                                     role == "last_frame" || role == "reference_image"
                           : kind == MediaKind::Video
                               ? role.empty() || role == "reference" || role == "reference_video"
                               : role.empty() || role == "reference" || role == "reference_audio";
    if (!validRole) return maiCreativeInvalid("Reference role does not match its media type");
    attachments.push_back({kind, role, path});
    return std::nullopt;
}

const char* referenceRole(MediaKind kind) {
    return kind == MediaKind::Image   ? "reference_image"
           : kind == MediaKind::Video ? "reference_video"
                                      : "reference_audio";
}

std::optional<MaiToolResult> validateH3OutputSpec(const Json& args, const std::string& model) {
    if (model != "MiniMax-H3" && model != "MiniMax-H3-Max")
        return maiCreativeInvalid("Select MiniMax-H3 or MiniMax-H3-Max");
    if (!args.contains("duration"))
        return maiCreativeInvalid("Missing top-level duration for paid video generation");
    if (!args["duration"].is_number_integer())
        return maiCreativeInvalid("duration must be an integer");
    if (!args.contains("resolution"))
        return maiCreativeInvalid("Missing top-level resolution for paid video generation");
    if (!args["resolution"].is_string()) return maiCreativeInvalid("resolution must be a string");
    const int duration = args["duration"].get<int>();
    const std::string resolution = value(args, "resolution");
    if (model == "MiniMax-H3" &&
        (duration < 4 || duration > 15 || (resolution != "768P" && resolution != "2K")))
        return maiCreativeInvalid(
            "MiniMax-H3 supports 4-15 seconds at 768P or 2K; "
            "native 1080P and 720P are unavailable");
    if (model == "MiniMax-H3-Max" &&
        (duration < 5 || duration > 15 || (resolution != "480P" && resolution != "768P")))
        return maiCreativeInvalid("MiniMax-H3-Max supports 5-15 seconds at 480P or 768P");
    return std::nullopt;
}

std::optional<MaiToolResult> prepareH3Request(const Json& args, const std::string& model,
                                              std::string prompt, const std::string& key,
                                              const std::string& caBundle,
                                              const MaiCreativeMediaUploadProvider& uploadMedia,
                                              const MaiToolContext& context, bool localOnly,
                                              Json& body, std::string& inputReference) {
    std::vector<Attachment> attachments;
    const std::string first =
        value(args, "first_frame").empty() ? value(args, "image_path") : value(args, "first_frame");
    const std::string last = value(args, "last_frame").empty() ? value(args, "last_frame_path")
                                                               : value(args, "last_frame");
    if (!first.empty()) attachments.push_back({MediaKind::Image, "first_frame", first});
    if (!last.empty()) attachments.push_back({MediaKind::Image, "last_frame", last});
    const std::string referenceVideo =
        !value(args, "reference_video").empty()        ? value(args, "reference_video")
        : !value(args, "reference_video_path").empty() ? value(args, "reference_video_path")
                                                       : value(args, "video_path");
    if (!referenceVideo.empty())
        attachments.push_back({MediaKind::Video, "reference_video", referenceVideo});
    const std::string referenceAudio = !value(args, "reference_audio").empty()
                                           ? value(args, "reference_audio")
                                           : value(args, "reference_audio_path");
    if (!referenceAudio.empty())
        attachments.push_back({MediaKind::Audio, "reference_audio", referenceAudio});
    if (args.contains("local_image_paths")) {
        if (!args["local_image_paths"].is_array())
            return maiCreativeInvalid("local_image_paths must be an array");
        for (const Json& path : args["local_image_paths"]) {
            if (!path.is_string())
                return maiCreativeInvalid("local_image_paths entries must be strings");
            attachments.push_back({MediaKind::Image, "reference_image", path.get<std::string>()});
        }
    }
    if (args.contains("content")) {
        if (!args["content"].is_array()) return maiCreativeInvalid("content must be an array");
        for (const Json& item : args["content"]) {
            if (!item.is_object()) return maiCreativeInvalid("content entries must be objects");
            if (item.contains("role") && !item["role"].is_string())
                return maiCreativeInvalid("content role must be a string");
            const std::string type = value(item, "type");
            if (type == "text") {
                const std::string text = value(item, "text");
                if (text.empty()) return maiCreativeInvalid("text content cannot be empty");
                if (!prompt.empty()) prompt += "\n";
                prompt += text;
                continue;
            }
            const MediaKind kind = type == "image_url"   ? MediaKind::Image
                                   : type == "video_url" ? MediaKind::Video
                                                         : MediaKind::Audio;
            if (type != "image_url" && type != "video_url" && type != "audio_url")
                return maiCreativeInvalid(
                    "content type must be text, image_url, video_url, or audio_url");
            if (auto error =
                    appendAttachment(attachments, kind, value(item, "role"), value(item, "path")))
                return error;
        }
    }
    const std::size_t imageCount = static_cast<std::size_t>(
        std::count_if(attachments.begin(), attachments.end(),
                      [](const Attachment& item) { return item.kind == MediaKind::Image; }));
    const bool referenceMode =
        imageCount > 1 ||
        std::any_of(attachments.begin(), attachments.end(), [](const Attachment& item) {
            return item.kind != MediaKind::Image || item.role == "reference" ||
                   item.role == "reference_image";
        });
    for (Attachment& item : attachments) {
        if (item.role == "reference") item.role = referenceRole(item.kind);
        if (item.role.empty())
            item.role = item.kind == MediaKind::Image && !referenceMode ? "first_frame"
                                                                        : referenceRole(item.kind);
    }
    if (auto error = validateH3OutputSpec(args, model)) return error;
    const int duration = args["duration"].get<int>();
    const std::string resolution = value(args, "resolution");
    if (prompt.empty() || prompt.size() > 8000)
        return maiCreativeInvalid("A text instruction is required and must be at most 8000 bytes");
    std::size_t firstCount = 0;
    std::size_t lastCount = 0;
    std::size_t referenceImages = 0;
    std::size_t referenceVideos = 0;
    std::size_t referenceAudios = 0;
    for (const Attachment& item : attachments) {
        if (item.path.empty()) return maiCreativeInvalid("Reference path is empty");
        if (item.role == "first_frame") ++firstCount;
        if (item.role == "last_frame") ++lastCount;
        if (item.role == "reference_image") ++referenceImages;
        if (item.role == "reference_video") ++referenceVideos;
        if (item.role == "reference_audio") ++referenceAudios;
    }
    if (firstCount > 1 || lastCount > 1 || referenceImages > 9 || referenceVideos > 3 ||
        referenceAudios > 3)
        return maiCreativeInvalid("Reference count exceeds the H3 input limit");
    const bool hasFrames = firstCount != 0 || lastCount != 0;
    const bool hasReferences = referenceImages != 0 || referenceVideos != 0 || referenceAudios != 0;
    if (hasFrames && hasReferences)
        return maiCreativeInvalid("Frame control and multimodal references cannot be combined");
    const std::string ratio = value(args, "ratio");
    const bool validRatio = ratio == "adaptive" || ratio == "21:9" || ratio == "16:9" ||
                            ratio == "4:3" || ratio == "1:1" || ratio == "3:4" || ratio == "9:16";
    if ((!ratio.empty() && !validRatio) ||
        (!hasFrames && !hasReferences && (ratio.empty() || ratio == "adaptive")))
        return maiCreativeInvalid("Confirm a concrete ratio for text-to-video");
    const std::string expansion = value(args, "prompt_expansion_mode");
    if (!expansion.empty() &&
        (model != "MiniMax-H3-Max" ||
         (expansion != "disabled" && expansion != "balanced" && expansion != "quality")))
        return maiCreativeInvalid("prompt_expansion_mode requires MiniMax-H3-Max");
    Json content = Json::array({Json{{"type", "text"}, {"text", prompt}}});
    for (const Attachment& item : attachments) {
        if (inputReference.empty()) inputReference = item.path;
        const char* type = item.kind == MediaKind::Image   ? "image_url"
                           : item.kind == MediaKind::Video ? "video_url"
                                                           : "audio_url";
        if (localOnly) {
            LocalReference local;
            if (auto error = inspectReference(item, context, local)) return error;
            content.push_back(Json{{"type", type},
                                   {"role", item.role},
                                   {"path", item.path},
                                   {"file_size_bytes", local.size}});
            continue;
        }
        std::string reference;
        if (item.kind != MediaKind::Audio) {
            // 图像、视频必须先传私有 OSS；上传失败时停在付费提交之前。
            LocalReference local;
            if (auto error = inspectReference(item, context, local)) return error;
            if (!uploadMedia)
                return maiCreativeFailure(MaiErrorCode::NotConfigured, "upload_not_configured",
                                          "Private OSS media upload is not configured");
            const auto uploaded = uploadMedia(item.path, context);
            if (!uploaded)
                return maiCreativeFailure(uploaded.error().code(), "upload_failed",
                                          uploaded.error().message());
            reference = uploaded.value();
            if (!maiCreativeHttpsUrl(reference))
                return maiCreativeFailure(MaiErrorCode::Protocol, "upload_failed",
                                          "OSS returned no HTTPS media URL");
        } else {
            const UploadedReference uploaded = uploadReference(item, key, caBundle, context);
            if (uploaded.error) return uploaded.error;
            reference = uploaded.reference;
        }
        content.push_back(
            Json{{"type", type}, {type, Json{{"url", reference}}}, {"role", item.role}});
    }
    body = {{"model", model},
            {"content", std::move(content)},
            {"duration", duration},
            {"resolution", resolution},
            {"ratio", hasFrames       ? "adaptive"
                      : ratio.empty() ? "adaptive"
                                      : ratio}};
    if (!expansion.empty()) {
        body["extra"] = Json{{"prompt_expansion_mode", expansion}};
    }
    return std::nullopt;
}

std::string providerFailure(const MaiToolResult& result) {
    if (!result.hasError()) return {};
    const Json error = Json::parse(result.error().message(), nullptr, false);
    if (!error.is_object() || value(error, "code") != "provider_error") return {};
    const Json code = error.value("provider_code", Json{});
    const int number = code.is_number_integer() ? code.get<int>() : 0;
    const int status = error.value("http_status", Json{}).is_number_integer()
                           ? error["http_status"].get<int>()
                           : 0;
    const std::string message = value(error, "message");
    const bool sensitive =
        message.find("(1026)") != std::string::npos || message.find("(1027)") != std::string::npos;
    const bool terminal = number == 1004 || number == 1026 || number == 1027 || number == 2013 ||
                          sensitive || status == 401 || status == 403 || status == 422;
    return terminal ? (message.empty() ? "MiniMax provider rejected task" : message)
                    : std::string{};
}

class MaiMiniMaxMediaTool final : public MaiTool {
public:
    MaiMiniMaxMediaTool(bool video, MaiMiniMaxApiKeyProvider key, std::string caBundle,
                        MaiCreativeMediaUploadProvider uploadMedia)
        : mVideo(video),
          mKey(std::move(key)),
          mCaBundle(std::move(caBundle)),
          mUploadMedia(std::move(uploadMedia)) {}

    std::string name() const override {
        return mVideo ? "minimax_video" : "minimax_image";
    }
    std::string description() const override {
        // 视频分支英文描述的对应含义：MiniMax H3 为付费视频工具，接文本、首尾帧，
        // 也接图片/视频/音频的多模态参考。content[] 的图片角色用 reference_image；
        // 工具兼容 reference 或省略角色的旧写法，文本条目的 role 会忽略。
        // duration/resolution 位于顶层；H3 支持 4–15 秒、768P/2K；H3 Max 支持
        // 5–15 秒、480P/768P。两者都无原生 1080P，若用户必须要该规格，先说明
        // “2K 生成后本地降采样”再付费确认。H3 Max 可用 model 显式选择。
        // 严格首尾帧不可与多模态参考混用；纯文生视频必须给具体画幅。
        // 参考视频只用来引导生成，当前没有承诺逐像素保留原视频的专用编辑模式。
        // validate 只做本地校验不上传不扣费，delegate 确认后生成，continue 查任务；
        // delete 仅移除已交付的本地任务记录，没有已核实的云端取消接口。
        // 终态结果会自动回灌主模型。图片分支 image-01 支持文生图及角色参考图，
        // discover 查能力，delegate 需付费确认且在本次调用内完成生成。
        return mVideo ? "MiniMax H3 video specialist. Call discover before model selection; "
                        "validate checks a plan locally without upload or charge. H3 creates "
                        "4-15 seconds at native 768P/2K; H3 Max creates 5-15 seconds at "
                        "480P/768P. Neither natively outputs 1080P. Text-only, one first "
                        "frame or first+last frames, and multimodal reference generation are "
                        "wired. Multimodal limits: up to nine images, three videos, three "
                        "audio clips; strict first/last frames cannot mix with references. "
                        "content[] media roles are reference_image/reference_video/"
                        "reference_audio; local image and video paths go through private OSS. "
                        "A reference video is not a guaranteed source-video edit. Text-only "
                        "requires a concrete ratio. Confirm model, duration, resolution and "
                        "ratio before paid delegate. Continue checks the existing task; delete "
                        "removes only a delivered local record. No verified cloud cancel API "
                        "is wired. For exact 1080P, explain 2K generation then local downscale."
                      : "MiniMax image-01 paid image specialist. Text-to-image and character "
                        "reference image generation are wired. Use discover or delegate after "
                        "confirmation; image generation completes in the delegate call.";
    }
    std::string parametersSchema() const override {
        // content[] 是 V2 多模态输入；image_path 等扁平字段保留单图便利写法。
        // duration、resolution 位于顶层；content 条目用 type/role/path 描述媒体。
        // 参数形状通过后，仍需执行层检查媒体角色、数量和模型规格组合。
        return mVideo
                   ? describeMiniMaxVideoSchema(
                         R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","validate","delegate","continue","delete"]},"model":{"type":"string","enum":["MiniMax-H3","MiniMax-H3-Max"]},"message":{"type":"string"},"context":{"type":"string"},"image_path":{"type":"string"},"first_frame":{"type":"string"},"last_frame_path":{"type":"string"},"last_frame":{"type":"string"},"local_image_paths":{"type":"array","items":{"type":"string"}},"reference_video_path":{"type":"string"},"reference_video":{"type":"string"},"video_path":{"type":"string"},"reference_audio_path":{"type":"string"},"reference_audio":{"type":"string"},"content":{"type":"array","items":{"type":"object","properties":{"type":{"type":"string","enum":["text","image_url","video_url","audio_url"]},"text":{"type":"string"},"path":{"type":"string"},"role":{"type":"string"}},"required":["type"]}},"duration":{"type":"integer"},"resolution":{"type":"string"},"ratio":{"type":"string"},"prompt_expansion_mode":{"type":"string"},"conversation_id":{"type":"string"},"parent_task_id":{"type":"string"},"poll_once":{"type":"boolean"}},"required":["action"]})")
                   : R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","delegate"]},"message":{"type":"string"},"context":{"type":"string"},"image_path":{"type":"string"},"ratio":{"type":"string"}},"required":["action"]})";
    }
    std::optional<MaiSpecialistInfo> specialistInfo() const override {
        const bool configured = mKey && !mKey().empty();
        const auto ready = configured ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                      : MaiSpecialistCapabilityStatus::NotConfigured;
        const auto mediaReady = !configured ? MaiSpecialistCapabilityStatus::NotConfigured
                                : mUploadMedia
                                    ? MaiSpecialistCapabilityStatus::ImplementedUnverified
                                    : MaiSpecialistCapabilityStatus::UploadNotConfigured;
        MaiSpecialistInfo info;
        info.toolName = name();
        info.modelId = mVideo ? "MiniMax-H3" : "image-01";
        info.configured = configured;
        if (mVideo) {
            // text_to_video：H3 为 4–15 秒，H3 Max 为 5–15 秒；
            // H3 V2 文生视频在当前账号做过一次实盘冒烟测试。
            info.capabilities.push_back(
                {"text_to_video", true, true, ready,
                 "H3: 4-15 seconds at 768P/2K. H3 Max: 5-15 seconds at "
                 "480P/768P. Text-only requires an explicit non-adaptive "
                 "ratio. H3 V2 text-to-video passed a live Mac smoke test"});
            // image_to_video：首帧决定画幅。底层本地校验可识别多种扩展名，
            // 但当前私有 OSS 签发器只已打通 JPG/PNG，不能向主模型宣称
            // WEBP/HEIC/HEIF 也已经端到端可用。
            info.capabilities.push_back(
                {"image_to_video", true, true, mediaReady,
                 "One first_frame/image_path uses source aspect ratio; local JPG/PNG uploads "
                 "through private OSS"});
            // first_last_frame_video：只接一张首帧和一张尾帧，比例仍由原图决定。
            info.capabilities.push_back(
                {"first_last_frame_video", true, true, mediaReady,
                 "One first and one last frame control the opening and ending frames; "
                 "cannot mix with multimodal reference images/videos/audio"});
            // multi_reference_video：最多九图、三视频、三音频，经文件上传进入 content。
            // 图片 role 可为 reference_image/reference，也可省略；Mac 共享核心的三图
            // 生成已实测，iOS 宿主交付尚未验证，不能误写成全平台已验收。
            info.capabilities.push_back(
                {"multi_reference_video", true, true, mediaReady,
                 "Up to nine reference images, three videos, and three audio clips. Images "
                 "and videos use private OSS; audio uses provider upload. content image role "
                 "reference_image or reference is accepted. Three-image H3 generation passed "
                 "on Mac; iOS host delivery remains unverified"});
            // native_audio_output：H3 冒烟测试产出 AAC；V2 API 没有独立的音频输出开关。
            info.capabilities.push_back(
                {"native_audio_output", true, true, ready,
                 "H3 smoke test produced AAC; the V2 API has no separate audio-output switch"});
            // two_k_video：H3 原生只有 768P/2K，绝无原生 1080P；H3 Max 为 480P/768P。
            info.capabilities.push_back(
                {"two_k_video", true, true, ready,
                 "MiniMax-H3 supports 768P or 2K, never native 1080P; H3 Max supports "
                 "480P or 768P"});
            // 参考视频是条件输入；没有单独的源视频编辑契约，不能承诺精确保留画面。
            info.capabilities.push_back(
                {"source_video_edit", false, false, MaiSpecialistCapabilityStatus::NotImplemented,
                 "Reference videos guide generation; this tool has no dedicated source-video "
                 "editing mode or pixel-preservation contract"});
            // 当前没有经过核实的 MiniMax 云端取消接口；不能假装停止本地轮询就停费。
            info.capabilities.push_back({"cloud_task_cancel", false, false,
                                         MaiSpecialistCapabilityStatus::NotImplemented,
                                         "No verified provider cancellation API is wired"});
            // 已交付记录可在本地删除，但云端任务和已下载 MP4 均保留。
            info.capabilities.push_back(
                {"local_record_delete", false, false,
                 MaiSpecialistCapabilityStatus::ImplementedUnverified,
                 "delete requires a delivered task; cloud output and downloaded MP4 remain"});
        } else {
            // text_to_image：image-01 文生图已在当前账号做过实盘冒烟测试。
            info.capabilities.push_back(
                {"text_to_image", true, true, ready,
                 "image-01 text generation completed a live account smoke test"});
            // character_reference_image：参考图必须含角色人物，仅支持小于 5 MB 的 PNG/JPEG。
            info.capabilities.push_back(
                {"character_reference_image", true, true, mediaReady,
                 "Reference image must contain a character; PNG/JPEG under 5 MB"});
        }
        return info;
    }
    bool requiresApproval(const std::string& argumentsJson) const override {
        return requiresPerCallApproval(argumentsJson);
    }
    bool requiresPerCallApproval(const std::string& argumentsJson) const override {
        const Json args = Json::parse(argumentsJson, nullptr, false);
        const std::string action = value(args, "action");
        if (mVideo && action == "delegate" && args.is_object()) {
            const std::string model =
                value(args, "model").empty() ? "MiniMax-H3" : value(args, "model");
            if (validateH3OutputSpec(args, model)) return false;
        }
        return action != "discover" && action != "validate" && action != "continue";
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        // validate 只核对本地字段和媒体文件；delegate 才上传并付费创建 H3 任务。
        // content[] 先按 type/role 归一化，完成后以原 task ID 查询和下载。
        const Json args = Json::parse(argumentsJson, nullptr, false);
        if (!args.is_object()) return maiCreativeInvalid("arguments must be a JSON object");
        // 模型即使带旧字段直调 execute，也要拒绝而非遗漏图片后付费提交。
        const Json schema = Json::parse(parametersSchema(), nullptr, false);
        for (auto field = args.begin(); field != args.end(); ++field) {
            if (!schema["properties"].contains(field.key()))
                return maiCreativeInvalid("unsupported MiniMax parameter: " + field.key());
        }
        for (const char* field :
             {"action", "model", "message", "context", "image_path", "first_frame",
              "last_frame_path", "last_frame", "reference_video_path", "reference_video",
              "video_path", "reference_audio_path", "reference_audio", "resolution", "ratio",
              "prompt_expansion_mode", "conversation_id", "parent_task_id"}) {
            if (args.contains(field) && !args[field].is_string())
                return maiCreativeInvalid(std::string(field) + " must be a string");
        }
        if (args.contains("duration") && !args["duration"].is_number_integer())
            return maiCreativeInvalid("duration must be an integer");
        if (args.contains("poll_once") && !args["poll_once"].is_boolean())
            return maiCreativeInvalid("poll_once must be boolean");
        const std::string action = value(args, "action");
        if (action == "discover") {
            const MaiSpecialistInfo info = *specialistInfo();
            Json capabilities = Json::array();
            for (const MaiSpecialistCapability& capability : info.capabilities)
                capabilities.push_back(
                    {{"id", capability.id},
                     {"model_support", capability.modelSupported},
                     {"api_support", capability.apiSupported},
                     {"tool_status", maiSpecialistCapabilityStatusToString(capability.status)},
                     {"limitation", capability.limitation}});
            return MaiToolResult::success(Json{{"tool_kind", "model_backed"},
                                               {"bound_model", info.modelId},
                                               {"configured", info.configured},
                                               {"local_validation", mVideo},
                                               {"capabilities", std::move(capabilities)},
                                               {"reply", "MiniMax capabilities listed"}}
                                              .dump());
        }
        if (action == "validate")
            return mVideo ? validate(args, context)
                          : maiCreativeInvalid("Local validation is only available for H3 video");
        if (mVideo && action == "delete") {
            // 删除仅是本地索引整理；尚在生成的云端任务不能通过此入口伪装成停止。
            const std::string id = value(args, "conversation_id");
            MaiSpecialistTask task;
            if (id.compare(0, 4, "spt_") != 0 || context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(id, context.sessionId, task) ||
                task.specialistName != name())
                return maiCreativeFailure(MaiErrorCode::NotFound, "not_found",
                                          "MiniMax video task was not found in this conversation");
            const MaiError removed =
                context.specialistTasks->deleteCompletedSpecialistTask(id, context.sessionId);
            if (removed)
                return maiCreativeFailure(removed.code(), "delete_unavailable", removed.message());
            return MaiToolResult::success(
                Json{{"conversation_id", id},
                     {"task_id", task.providerTaskId},
                     {"status", "local_record_deleted"},
                     {"provider_deleted", false},
                     {"reply", "Local MiniMax task record deleted; cloud job and output remain."}}
                    .dump());
        }
        if (action != "delegate" && action != "continue")
            return maiCreativeInvalid(
                "action must be discover, validate, delegate, continue, or delete for video");
        if (!mVideo && action == "continue")
            return maiCreativeInvalid("MiniMax image generation completes during delegate");
        if (args.contains("video_url"))
            return maiCreativeInvalid("Use workspace reference_video_path instead of video_url");
        const std::string key = mKey ? mKey() : std::string{};
        if (key.empty())
            return maiCreativeFailure(MaiErrorCode::NotConfigured, "not_configured",
                                      "MiniMax API key is missing in the host configuration");
        return action == "delegate" ? delegate(args, key, context)
                                    : continueTask(args, key, context);
    }

private:
    MaiToolResult validate(const Json& args, const MaiToolContext& context) const {
        if (context.root.empty()) return maiCreativeInvalid("Agent workspace is required");
        std::string model = value(args, "model");
        if (model.empty()) model = "MiniMax-H3";
        if (model != "MiniMax-H3" && model != "MiniMax-H3-Max")
            return maiCreativeInvalid("Select MiniMax-H3 or MiniMax-H3-Max");
        std::string prompt = value(args, "message");
        const std::string extra = value(args, "context");
        if (prompt.size() > 4000 || extra.size() > 4000)
            return maiCreativeInvalid("message/context must be at most 4000 bytes");
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        Json body;
        std::string inputReference;
        if (auto error = prepareH3Request(args, model, prompt, {}, mCaBundle, mUploadMedia, context,
                                          true, body, inputReference))
            return *error;
        return MaiToolResult::success(Json{
            {"status", "validated"},
            {"submitted", false},
            {"uploaded", false},
            {"charged", false},
            {"request_preview",
             std::move(body)}}.dump());
    }

    MaiToolResult delegate(const Json& args, const std::string& key,
                           const MaiToolContext& context) const {
        const std::string message = value(args, "message");
        const std::string extra = value(args, "context");
        if (message.size() > 4000 || extra.size() > 4000)
            return maiCreativeInvalid("message/context must be at most 4000 bytes");
        if (context.root.empty()) return maiCreativeInvalid("Agent workspace is required");
        std::string prompt = message;
        if (!extra.empty()) prompt += "\nRelevant context: " + extra;
        if (!mVideo) return delegateImage(args, key, prompt, context);
        std::string model = value(args, "model");
        if (model.empty()) model = "MiniMax-H3";
        if (model != "MiniMax-H3" && model != "MiniMax-H3-Max")
            return maiCreativeInvalid("Select MiniMax-H3 or MiniMax-H3-Max");
        Json body;
        std::string inputReference;
        if (auto error = prepareH3Request(args, model, prompt, key, mCaBundle, mUploadMedia,
                                          context, false, body, inputReference))
            return *error;
        prompt = value(body["content"][0], "text");
        const std::string endpoint = std::string(kApiBase) + "/v2/video_generation";
        const std::string payload = body.dump();
        const MaiCreativeHttpResult submitted =
            maiCreativeRequestJson(endpoint, key, mCaBundle, &payload, context);
        if (submitted.error) return *submitted.error;
        const Json response = Json::parse(submitted.body, nullptr, false);
        const std::string providerId = idValue(response, "task_id");
        if (!maiCreativeValidId(providerId))
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "MiniMax returned no task ID");
        const std::string lookupId =
            model == "MiniMax-H3" ? "h3_" + providerId : "h3max_" + providerId;
        const MaiToolResult initial = continueTask(
            Json{{"conversation_id", lookupId}, {"input_reference", inputReference}}, key, context);
        if (!initial.hasError()) {
            const Json progress = Json::parse(initial.output(), nullptr, false);
            if (value(progress, "status") == "failed")
                return maiCreativeFailure(MaiErrorCode::Network, "task_failed",
                                          value(progress, "reply"));
            if (value(progress, "status") == "succeeded") return initial;
        }
        Json output = {{"task_id", providerId},
                       {"conversation_id", lookupId},
                       {"status", "submitted"},
                       {"bound_model", model},
                       {"reply", "The MiniMax task has started. The app will report its result."}};
        if (initial.hasError())
            output["initial_check_warning"] =
                "The first status check failed; background status checks will retry.";
        if (!value(args, "image_path").empty() || !value(args, "first_frame").empty())
            output["ratio_policy"] = "The first frame controls the output aspect ratio.";
        if (context.specialistTasks != nullptr && !context.sessionId.empty()) {
            MaiSpecialistTask task;
            task.id = MaiIdGenerator::generate("spt_");
            task.ownerSessionId = context.sessionId;
            task.specialistName = name();
            task.parentTaskId = value(args, "parent_task_id");
            task.providerTaskId = lookupId;
            task.intent = message.empty() ? prompt : message;
            task.contextSummary = extra;
            task.inputReference = inputReference;
            task.created = MaiTime::getCurrentTime();
            const MaiError stored = context.specialistTasks->insertSpecialistTask(task);
            if (stored) {
                output["context_persistence_warning"] = stored.message();
                output["reply"] =
                    "The task was submitted but automatic notification could not "
                    "be scheduled. Continue using its provider task ID.";
            } else {
                output["conversation_id"] = task.id;
                output["specialist_task_id"] = task.id;
            }
        }
        return MaiToolResult::success(output.dump());
    }

    MaiToolResult delegateImage(const Json& args, const std::string& key, const std::string& prompt,
                                const MaiToolContext& context) const {
        if (prompt.empty() || prompt.size() > 4000)
            return maiCreativeInvalid("message is required for image generation");
        const std::string ratio = value(args, "ratio");
        if (ratio != "16:9" && ratio != "9:16" && ratio != "1:1")
            return maiCreativeInvalid("Confirm ratio as 16:9, 9:16, or 1:1");
        Json body = {{"model", "image-01"},
                     {"prompt", prompt},
                     {"aspect_ratio", ratio},
                     {"response_format", "url"},
                     {"n", 1}};
        const std::string imagePath = value(args, "image_path");
        if (!imagePath.empty()) {
            // image-01 的 image_file 接受 URL；不再把本地图转成 data URL。
            if (!mUploadMedia)
                return maiCreativeFailure(MaiErrorCode::NotConfigured, "upload_not_configured",
                                          "Private OSS media upload is not configured");
            const auto uploaded = mUploadMedia(imagePath, context);
            if (!uploaded)
                return maiCreativeFailure(uploaded.error().code(), "upload_failed",
                                          uploaded.error().message());
            std::string image = uploaded.value();
            if (!maiCreativeHttpsUrl(image))
                return maiCreativeFailure(MaiErrorCode::Protocol, "upload_failed",
                                          "OSS returned no HTTPS image URL");
            body["subject_reference"] =
                Json::array({Json{{"type", "character"}, {"image_file", std::move(image)}}});
        }
        const std::string payload = body.dump();
        const MaiCreativeHttpResult result = maiCreativeRequestJson(
            std::string(kApiBase) + "/v1/image_generation", key, mCaBundle, &payload, context);
        if (result.error) return *result.error;
        const Json response = Json::parse(result.body, nullptr, false);
        const Json data = response.value("data", Json::object());
        if (!data.is_object() || !data.contains("image_urls") || !data["image_urls"].is_array() ||
            data["image_urls"].empty() || !data["image_urls"][0].is_string())
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "MiniMax returned no image URL");
        MaiToolResult media = maiCreativeDownloadMedia(data["image_urls"][0].get<std::string>(),
                                                       false, "minimax", context, mCaBundle);
        if (media.hasError()) return media;
        Json output = Json::parse(media.output());
        output["status"] = "succeeded";
        output["bound_model"] = "image-01";
        output["reply"] = "MiniMax image is ready for review at the returned path.";
        return MaiToolResult::success(output.dump());
    }

    MaiToolResult continueTask(const Json& args, const std::string& key,
                               const MaiToolContext& context) const {
        const std::string conversationId = value(args, "conversation_id");
        if (!maiCreativeValidId(conversationId))
            return maiCreativeInvalid("conversation_id must be a valid task ID");
        std::string lookupId = conversationId;
        std::string inputReference = value(args, "input_reference");
        if (conversationId.compare(0, 4, "spt_") == 0) {
            MaiSpecialistTask previous;
            if (context.specialistTasks == nullptr ||
                !context.specialistTasks->getSpecialistTask(conversationId, context.sessionId,
                                                            previous) ||
                previous.specialistName != name())
                return maiCreativeFailure(MaiErrorCode::NotFound, "not_found",
                                          "MiniMax task was not found in this conversation");
            if (previous.status == MaiSpecialistTaskStatus::Failed &&
                previous.errorText.rfind("Status check failed: ", 0) != 0)
                return failed(conversationId, previous.providerTaskId, previous.errorText,
                              !previous.inputReference.empty());
            if (previous.status == MaiSpecialistTaskStatus::Succeeded &&
                !previous.outputPath.empty())
                return MaiToolResult::success(Json{
                    {"conversation_id", conversationId},
                    {"status", "succeeded"},
                    {"path", previous.outputPath},
                    {"reply", previous.finalText}}.dump());
            lookupId = previous.providerTaskId;
            inputReference = previous.inputReference;
        }
        const bool h3Max = lookupId.compare(0, 6, "h3max_") == 0;
        const bool h3 = h3Max || lookupId.compare(0, 3, "h3_") == 0;
        if (!h3)
            return terminalFailure(conversationId, lookupId,
                                   "Legacy Hailuo 2.3 task is no longer supported", context,
                                   inputReference);
        const std::string providerId = lookupId.substr(h3Max ? 6 : 3);
        if (!maiCreativeValidId(providerId))
            return maiCreativeInvalid("MiniMax provider task ID is invalid");
        const std::string url = std::string(kApiBase) + "/v2/query/video_generation/" + providerId;
        const MaiCreativeHttpResult result =
            maiCreativeRequestJson(url, key, mCaBundle, nullptr, context);
        if (result.error) {
            const std::string reason = providerFailure(*result.error);
            if (!reason.empty())
                return terminalFailure(conversationId, providerId, reason, context, inputReference);
            return *result.error;
        }
        const Json response = Json::parse(result.body, nullptr, false);
        const Json task = response.value("task", Json::object());
        if (!task.is_object())
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "MiniMax H3 returned no task");
        const std::string status = value(task, "status");
        if (status == "queued" || status == "running") return running(conversationId, providerId);
        if (status == "failed" || status == "cancelled") {
            const Json error = task.value("error", Json::object());
            std::string reason = value(error, "message");
            if (reason.empty()) reason = "MiniMax H3 generation failed";
            const std::string code = value(error, "code");
            if (!code.empty()) reason += " (" + code + ")";
            return terminalFailure(conversationId, providerId, reason, context, inputReference);
        }
        if (status != "succeeded")
            return maiCreativeFailure(MaiErrorCode::Protocol, "protocol",
                                      "MiniMax H3 returned unknown task status");
        const Json content = task.value("content", Json::object());
        const std::string mediaUrl = value(content, "url");
        return finish(mediaUrl, conversationId, providerId, h3Max ? "MiniMax-H3-Max" : "MiniMax-H3",
                      task, context);
    }

    MaiToolResult running(const std::string& conversationId, const std::string& providerId) const {
        return MaiToolResult::success(Json{{"conversation_id", conversationId},
                                           {"task_id", providerId},
                                           {"status", "running"},
                                           {"reply", "MiniMax video is still processing."}}
                                          .dump());
    }

    MaiToolResult failed(const std::string& conversationId, const std::string& providerId,
                         const std::string& reason, bool hasReference) const {
        const bool imageExplicit = reason.find("image sensitive") != std::string::npos ||
                                   reason.find("image safety") != std::string::npos;
        const bool textLabeled = reason.find("text sensitive") != std::string::npos;
        const std::string attribution = imageExplicit ? "provider_identified_image"
                                        : textLabeled ? "provider_labeled_text"
                                                      : "provider_unspecified";
        return MaiToolResult::success(Json{
            {"conversation_id", conversationId},
            {"task_id", providerId},
            {"status", "failed"},
            {"input_reference_present", hasReference},
            {"cause_attribution", attribution},
            {"reply", reason.empty()
                          ? "MiniMax generation failed"
                          : reason}}.dump());
    }

    MaiToolResult terminalFailure(const std::string& conversationId, const std::string& providerId,
                                  const std::string& reason, const MaiToolContext& context,
                                  const std::string& inputReference) const {
        std::string explanation = reason.empty() ? "MiniMax generation failed" : reason;
        const bool moderation = explanation.find("1026") != std::string::npos ||
                                explanation.find("sensitive") != std::string::npos;
        if (moderation && !inputReference.empty() &&
            explanation.find("Reference media was included") == std::string::npos) {
            explanation +=
                " Reference media was included. The provider did not establish "
                "whether the media or text caused this rejection.";
        }
        if (conversationId.compare(0, 4, "spt_") == 0 && context.specialistTasks != nullptr) {
            (void)context.specialistTasks->finishSpecialistTask(
                conversationId, context.sessionId, MaiSpecialistTaskStatus::Failed, explanation, {},
                MaiTime::getCurrentTime());
        }
        return failed(conversationId, providerId, explanation, !inputReference.empty());
    }

    MaiToolResult finish(const std::string& url, const std::string& conversationId,
                         const std::string& providerId, const std::string& model, const Json& task,
                         const MaiToolContext& context) const {
        MaiToolResult media = maiCreativeDownloadMedia(url, true, "minimax", context, mCaBundle);
        if (media.hasError()) return media;
        Json output = Json::parse(media.output());
        output["conversation_id"] = conversationId;
        output["task_id"] = providerId;
        output["status"] = "succeeded";
        output["bound_model"] = model;
        if (!value(task, "ratio").empty()) output["ratio"] = value(task, "ratio");
        if (!value(task, "resolution").empty()) output["resolution"] = value(task, "resolution");
        output["reply"] = "MiniMax video is ready for review at the returned path.";
        return MaiToolResult::success(output.dump());
    }

    bool mVideo;
    MaiMiniMaxApiKeyProvider mKey;
    std::string mCaBundle;
    MaiCreativeMediaUploadProvider mUploadMedia;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiMiniMaxVideoTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath,
                                                 MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiMiniMaxMediaTool>(true, std::move(apiKey), std::move(caBundlePath),
                                                 std::move(uploadMedia));
}

std::unique_ptr<MaiTool> makeMaiMiniMaxImageTool(MaiMiniMaxApiKeyProvider apiKey,
                                                 std::string caBundlePath,
                                                 MaiCreativeMediaUploadProvider uploadMedia) {
    return std::make_unique<MaiMiniMaxMediaTool>(false, std::move(apiKey), std::move(caBundlePath),
                                                 std::move(uploadMedia));
}
