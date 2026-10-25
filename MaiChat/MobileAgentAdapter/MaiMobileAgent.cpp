// 文件作用：移动端 C++ Agent 适配层；MaiMobileAgent 负责移动 Agent 生命周期、工具注册和宿主回调，本文件给出其实现。
// 直接依赖：MaiMobileHostTools.h、MaiAgent.h、MaiAgentSendMediaTool.h、MaiAppStorageTool.h；实现与这些接口协作。
#include "MaiMobileAgent.h"
#include "MaiMobileHostTools.h"

#include "MaiAgent.h"
#include "MaiAgentSendMediaTool.h"
#include "MaiAppStorageTool.h"
#include "MaiArkAssetTools.h"
#include "MaiArkMediaTools.h"
#include "MaiCurlTools.h"
#include "MaiCvImageTools.h"
#include "MaiCvVideoAnalysis.h"
#include "MaiCvVideoTools.h"
#include "MaiFfmpegTools.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiFileTools.h"
#include "MaiGlmMediaTools.h"
#include "MaiMiniMaxMediaTools.h"
#include "MaiModelStudioTools.h"
#include "MaiNetworkIpTool.h"
#include "MaiOpenAiClient.h"
#include "MaiOpenSslCliTool.h"
#include "MaiOpenSslTool.h"
#include "MaiPdfTool.h"
#include "MaiQuestionTool.h"
#include "MaiSqliteStore.h"
#include "MaiSshTool.h"
#include "MaiSystemResourcesTool.h"
#include "MaiTemporalWatermark.h"
#include "MaiTemporalWatermarkTool.h"
#include "MaiTimeTool.h"
#include "MaiTodoWriteTool.h"
#include "MaiVideoGeometryTool.h"
#include "MaiVideoMatting.h"
#include "MaiVideoMattingTool.h"
#include "MaiViewImageTool.h"
#include "MaiZlibTool.h"
#include "mai_fftools_embed.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

using Json = nlohmann::json;

namespace {

constexpr auto kMarkdownBaseInstructions =
    // 工具名和 description 只用于初筛；推荐前必须逐个调用候选视频工具的
    // discover， 读取 tool_status 和输入组合限制。厂商宣称支持不等于当前 App
    // 已接通。
    // 付费视频任务再确认模型及型号；遇到失败先判断原因并保持用户已选方案。
    "Video generation rule: before any paid video delegate, use discover to "
    "identify configured specialists with implemented capabilities for this "
    "request. Read the tool description for initial routing, then call "
    "discover "
    "on each plausible video specialist before recommending one. Compare the "
    "actual tool_status and input combination limits; model_support or "
    "api_support alone does not mean the App can execute a path. Use the JSON "
    "schema for exact fields. Do not infer a missing feature from a model "
    "name, "
    "marketing page, or another model's limits. If the user has not chosen a "
    "video model for this task, show "
    "suitable names and task-specific differences and ask the user to choose; "
    "confirm the sole suitable model too. Do not choose or switch video models "
    "silently. When one specialist offers multiple billable model variants, "
    "show their supported duration and resolution and confirm the variant too. "
    "Reuse a choice already made for this task, but ask before any "
    "replacement model's paid call. A model choice is separate from paid-call "
    "approval. Classify an input rejection before changing anything. If the "
    "cause is ambiguous, inspect the actual image and prompt without claiming "
    "which one failed. "
    // 人物替换应直接规划视频编辑；水印、字幕和片尾不是默认预处理任务。
    // 用户明确要求去水印时才用独立工具，一次处理后回到原目标，避免反复探测。
    "When the user asks to replace a person in a source video, keep the source "
    "video and references intact and plan the selected video model's edit "
    "path. "
    "Do not remove watermarks, subtitles, or end cards as an unrequested "
    "preparation step. If the user explicitly requests watermark removal, use "
    "the dedicated media_remove_watermark tool, if available, once with all "
    "measured regions, "
    "time windows, and clean reference timestamps. Inspect enough source "
    "frames "
    "to confirm moving positions and foreground overlap. Do not loop through "
    "FFmpeg/OpenCV repair calls. If that tool is unavailable, state the "
    "capability gap. After one result, review it and return to the original "
    "task. "
    // 图片与视频专业模型的本地素材统一走私有 OSS；上传故障不能回退为 Base64。
    "For image and video specialist tools, local media paths are upload "
    "inputs: "
    "the tool sends short-lived private OSS HTTPS URLs to the provider. If OSS "
    "upload fails, stop before paid submission and report the upload stage. Do "
    "not retry by embedding local image or video bytes as Base64. "
    // 可灵权限已开通，只走百炼入口并复用万相北京地域 Key；仍要确认具体型号。
    "For Kling video, use kling_bailian_video with the Beijing Wan API Key and "
    "Workspace. Confirm its Turbo, "
    "Standard, "
    "or Omni variant before paid submission. "
    "For Kling images use kling_bailian_image with the same Beijing "
    "credentials; "
    "for Wan images use wan_image. "
    // MiniMax image-01 的图生图是人物主体参考，不是任意局部修图。
    "For MiniMax images use minimax_image: no reference is text-to-image; one "
    "image_path or image_url is character-reference image-to-image. Do not "
    "describe "
    "that path as pixel-preserving editing or use it for arbitrary object "
    "replacement. "
    // 旧专用入口已停止新任务。Wan3 使用一张人物参考图和一段视频，
    // 通过提示词分别引导换人或动作模仿，但不能保证逐帧还原或严格保留背景。
    // 本地源视频先测时长，超过 Wan3 的 15 秒上限不能静默截短或付费提交。
    "For Wan person replacement use wan_video mode=edit, set video_path to the "
    "source "
    "video and local_image_paths to an array containing one person image. In "
    "message, "
    "ask to replace Video 1's person with Image 1 while preserving the scene. "
    "For "
    "motion guided by a source video use wan_video mode=reference with the "
    "same media "
    "fields; ask Image 1's person to follow Video 1's motion and expressions "
    "while "
    "keeping Image 1's background. These "
    "are prompt-guided approximations, not guaranteed "
    "frame-accurate replacement or motion transfer. Measure "
    "source_duration_seconds "
    "with ffprobe: Wan3 accepts at most 15 seconds of reference video, and "
    "input plus output must be at most 30 seconds. Use ratio=adaptive to "
    "preserve framing unless the user requests another ratio. Do not silently "
    "trim longer source footage. Use only wan_video for new Wan person "
    "replacement or "
    "motion-guided generation. "
    // Wan3 视频延长用 reference_video 和明确延长意图；比例必须 adaptive，
    // 付费前先测源片时长并核对输入与输出合计不超过 30 秒。
    "For Wan3 extension use wan_video mode=extend with one source video, an "
    "explicit "
    "extension direction in the message, ratio=adaptive, and measured "
    "source_duration_seconds. Input plus requested output must be at most 30 "
    "seconds. "
    // 方舟账号余额不足时暂停 Seedance 和 Seedream 新生成，历史任务仍可查询取回。
    "Seedance video and Seedream image generation are temporarily disabled "
    "because the Ark account balance is insufficient. Do not choose them for "
    "new tasks or prepare new media for them. Previously submitted Seedance "
    "tasks still receive background status checks and downloaded results. "
    "Preserve their task IDs; do not call the unlisted specialist tool. "
    // AIGC 组只用于虚拟素材；可识别真人必须先由本人完成 H5 授权。
    "AIGC groups are only for fictional or virtual subjects. If any selected "
    "photo or video contains a recognizable natural person, use an existing "
    "LivenessFace group or call ark_assets begin_real_validation, give the H5 "
    "link to that person, and resolve its GroupId with get_real_validation. "
    "Do not infer authorization from pixels, put a real person in an AIGC group, "
    "or call an Active AIGC asset authorized. Each LivenessFace group belongs to "
    "one person; multi-person media can fail consistency review. Preserve every "
    "selected input and report Processing, Active, or Failed accurately. "
    // 资产服务故障是传输问题；保留原方案并重试，不能误判成内容审核失败。
    "Do not treat an ark_assets transport error as an image moderation "
    "decision. "
    "For an existing Seedance task, keep its task ID and original references "
    "if the asset service is temporarily unreachable; do not submit a duplicate. "
    // 仅在允许的前提下处理图像：轻度处理优先，油画化是兜底；记录损失并补偿描述。
    "Do not default to "
    "flipping or oil-painting a real face to evade review. For other image "
    "issues, if an edit "
    "is allowed and still serves the user's goal, preserve the original, make "
    "a light "
    "FFmpeg/OpenCV edit, preview it, "
    "and retry the selected model with new paid approval. If image-related "
    "rejection persists, consider an FFmpeg oil-paint-style derivative and "
    "describe permitted details lost in processing. Do not switch models "
    "before these bounded same-model attempts. If the provider explicitly "
    "forbids a recognizable real-person reference without an authorized asset "
    "and the user needs that person's identity, do not transform the photo to "
    "hide the face and reconstruct it in the prompt. Keep the selected model; "
    "briefly offer its authorized-asset path or a visibly fictional character. "
    "Offer another model only if the user wants the original person's "
    "likeness, "
    "and ask before switching. Do not claim the account has no authorized "
    "portrait assets unless a tool actually checked; ask for an asset ID. "
    "Avoid lengthy technical error narration. "
    // 排障日志只在用户要求时导出到工作区，工具返回文件卡片；不把日志正文
    // 粘进会话，也不代替用户发送给联系人。
    "When the user asks to export troubleshooting logs, call "
    "mobile_export_diagnostics once and tell them to use the resulting "
    "shareable file card. Do not paste the log contents into the chat or "
    "send the file to a contact automatically. "
    // 消息用真正的 Markdown 换行；图像附件直接看，缺 OS 权限先申请再重试。
    "Write user-facing responses in valid GitHub-Flavored Markdown. Preserve "
    "real line breaks. "
    "For tables, put the header, separator, and every row on separate lines, "
    "with a blank line "
    "before and after the table. Use headings, lists, fenced code blocks, and "
    "tables only when "
    "they improve readability. Never emit table pipes as one continuous line. "
    // 用户消息自带的图像已作为视觉输入；相册编辑必须导出原件，预览图不能当源图。
    "Images included "
    "in a user message are already available as visual input; analyze them "
    "directly and do not "
    "call the read tool for image files. When a mobile task lacks an OS "
    "permission, call "
    "mobile_request_permission for that capability and retry once if granted. "
    "Android mobile_dial_phone requests CALL_PHONE itself; if the user denies it, "
    "do not prompt again for the same request. "
    "A limited grant "
    "may still allow the operation, such as reading selected photos or using "
    "approximate "
    "location; retry once when that access is sufficient. If the OS reports "
    "settings_required, "
    "tell the user which permission to enable instead of retrying in a loop. "
    "For any gallery "
    "photo edit, use mobile_export_photo_original as the input; "
    "mobile_read_photo returns a "
    "bounded JPEG preview that loses resolution and wide-gamut metadata. "
    "When the user writes in Chinese, write the final answer and any reasoning "
    "text exposed by the model in Simplified Chinese. Preserve code, paths, "
    "commands, and quoted tool output in their original form. "
    "Do not invent model quality, policy, or price claims. "
    "Use mobile_dial_phone only when the user explicitly asks to call. On Android, "
    "the operating system may ask once for CALL_PHONE permission, then the call "
    "starts without a second MaiChat approval. On iOS, the Phone app always asks "
    "the user to confirm. Never claim a call connected based on a requested or "
    "opened status. mobile_compose_sms only opens the iOS Messages composer; "
    "the user decides whether to send it. "
    // 付费调用前展示人物、场景和规格；闸门是最终确认，不能悄悄改时长或分辨率。
    "Before calling delegate or revise on any paid image or video generation "
    "specialist, "
    "show the user the concrete plan and let the tool's per-call approval be "
    "the final "
    "confirmation. For video, name subject, clothing, scene, action, duration, "
    "resolution, "
    "and aspect ratio. Ask for missing duration, resolution, or aspect ratio "
    "instead of "
    "silently using defaults. Read the selected specialist's actual duration "
    "range; "
    "do not assume a universal 10-second limit or shorten a longer request. "
    "Match the paid call to the shown plan: preserve "
    "the intended subject, clothing, scene, action, reference count, duration, "
    "aspect ratio, and the provider's actual billable resolution. Check "
    "discover or local validation before approval; never pass an unsupported "
    "resolution or silently change it. If exact output resolution requires "
    "local conversion, explain both steps before approval. If the user "
    "rejects the approval, do not submit "
    "or retry. "
    // 语义创作交给生成模型；裁剪、几何、色彩和速度调整优先使用本地工具。
    "Use video/image model tools for semantic changes such as person, clothes, "
    "scene, or "
    "action. Prefer local FFmpeg/OpenCV for geometry, crop, color, "
    "stabilization, frame "
    "joining, or speed changes so identity pixels can remain untouched. Reuse "
    "an existing "
    "cloud task ID when checking progress; do not submit a paid duplicate. "
    // 取消与删除必须走已保存的任务上下文；取消仅限供应商仍在排队的任务。
    // 百炼没有云端视频任务删除 API，其 delete 只移除已交付的本地记录。
    "When the user asks to cancel a video job, call an available specialist's "
    "cancel action with its saved conversation_id. Model Studio can cancel only "
    "queued/PENDING jobs; if the provider says it has started, preserve the task "
    "and continue polling. Previously submitted Seedance jobs still receive "
    "background checks, but their unlisted tool cannot be called for manual "
    "cancellation. Wan and Bailian Kling delete only the delivered local task "
    "record, leaving cloud results and downloaded videos intact. GLM and "
    "MiniMax also allow local-record deletion after delivery, but no verified "
    "provider cancel API is wired for them; never claim a local stop canceled "
    "their cloud job. "
    // 多图任务先查模型上限；超出时用图像融合或可见拼接，让每张输入都进入流程。
    "For multiple user images, inspect the chosen video specialist's input "
    "limits and never "
    "silently drop an image. If the limit is exceeded, discover an available "
    "image specialist for semantic fusion or use FFmpeg for a visible layout, then show the plan "
    "before the paid "
    "call. On task failure, inspect the provider stage and reason, repair "
    "technical inputs "
    "when possible, and seek a new per-call approval before any paid retry. Do "
    "not loop on "
    "a provider content rejection or imply that an ambiguous error identifies "
    "one input. "
    // 子工具失败先由主模型处理；欠费找替代能力，状态未知保留任务 ID
    // 防重复扣费。
    "Treat a specialist error as internal work to resolve, not an immediate "
    "user-facing "
    "answer. Keep the original task and media references. If one provider "
    "lacks balance or "
    "access, discover another capable configured specialist and continue the "
    "task there only after the user confirms a replacement video model when "
    "this is video generation; "
    "never ask the user to recharge while a practical alternative remains. If "
    "an accepted "
    "task's status is unknown, preserve its task ID and do not silently submit "
    "a duplicate. "
    "After delivering the result, you may briefly explain any provider change. "
    "Only if no "
    "viable route remains should you tell the user why the task could not be "
    "completed. "
    // 模糊图像审核先辨别实际输入，再在同一模型内有界处理和重试，换模型放最后。
    "For ambiguous image-related moderation failures, preserve the original "
    "and inspect the actual image and prompt. Try a light permitted image-tool "
    "edit first, "
    "then retry the same provider after a new paid approval. Only if that "
    "fails and image evidence remains, try a stronger FFmpeg oil-paint-style "
    "edit as the "
    "last image-processing step. Preview each derivative, record what visual "
    "details were lost, and describe permitted details accurately in the "
    "next prompt so the result stays close to the user's goal. Do not infer "
    "an image problem from an ambiguous error, disguise explicitly prohibited "
    "material, reconstruct a prohibited identity, or promise an edit will pass "
    "moderation. Consider another "
    "provider only after bounded same-provider attempts fail.";

std::string encodeBase64(const std::string& input) {
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second = offset + 1 < input.size() ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto third = offset + 2 < input.size() ? static_cast<unsigned char>(input[offset + 2]) : 0;
        output.push_back(kAlphabet[first >> 2]);
        output.push_back(kAlphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(offset + 1 < input.size() ? kAlphabet[((second & 0x0F) << 2) | (third >> 6)] : '=');
        output.push_back(offset + 2 < input.size() ? kAlphabet[third & 0x3F] : '=');
    }
    return output;
}

char* ownedResponse(const std::string& response) {
    char* result = static_cast<char*>(std::malloc(response.size() + 1));
    if (result) std::memcpy(result, response.c_str(), response.size() + 1);
    return result;
}

}  // namespace

// 这是移动端适配器，JSON 只在语言边界，MaiAgent 的公开接口仍是领域对象。
struct MaiMobileAgent {
    std::mutex mutex;
    bool dirty = true;
    bool structureChanged = true;
    std::string cachedSession;
    int cachedLimit = -1;
    std::vector<MaiMessage> cachedMessages;
    std::unordered_map<std::string, std::string> pendingDeltas;
    std::unordered_map<std::string, std::string> liveParts;
    std::unordered_map<std::string, std::string> errors;
    std::string workspace;
    std::string model;
    MaiModelConfig suggestionConfig;
    bool configured = false;
    const void* ortApiBase = nullptr;
    std::shared_ptr<MaiMobileHostDispatcher> hostTools = makeMaiMobileHostDispatcher();
    // 最后销毁 agent：回调捕获的字段必须活到工作线程退出。
    std::unique_ptr<MaiAgent> agent;

    ~MaiMobileAgent() {
        agent.reset();
        clearMaiMobileHostToolHandler(hostTools);
    }

    static std::string result(MaiResult<std::string> value) {
        if (!value) throw std::runtime_error(value.error().message());
        return value.value();
    }

    bool anyBusy() const {
        if (!agent) return false;
        for (const auto& session : agent->listSessions())
            if (agent->isBusy(session.id)) return true;
        return false;
    }

    // 作用：按宿主配置装配移动 Agent 的模型、工具和工作区能力。
    // 先校验外部输入，再更新本地状态；失败路径不能覆盖已有有效结果。
    void configure(const Json& request) {
        // 切换主模型时不能中断正在运行的任务；先创建共享存储，再配置线协议和工具集。
        if (anyBusy()) throw std::runtime_error("Stop active tasks before changing the model.");
        auto store = makeMaiSqliteStore(request.at("database").get<std::string>());
        if (!store) throw std::runtime_error(store.error().message());
        MaiModelConfig config;
        config.baseUrl = request.at("baseUrl").get<std::string>();
        config.apiKey = request.value("apiKey", "");
        config.caBundlePath = request.value("caBundle", "");
        // Responses/Chat 是主模型线协议，和 Seedance 等专业工具的供应商 API 无关。
        const std::string wire = request.value("wire", std::string{});
        if (wire == "responses" || (wire.empty() && request.value("model", std::string{}) == "deepseek-flash"))
            config.wire = MaiWireApi::Responses;
        else if (!wire.empty() && wire != "chat_completions")
            throw std::runtime_error("Unsupported model wire protocol.");
        MaiFfmpegEngine ffmpegEngine{mai_ffmpeg_execute,       mai_ffmpeg_set_cancel_check,
                                     mai_ffprobe_execute,      mai_ffprobe_set_cancel_check,
                                     mai_fftools_set_log_sink, mai_fftools_error_string};
        config.prepareImage = makeMaiFfmpegModelImagePreparer(ffmpegEngine);
        suggestionConfig = config;
        auto tools = std::make_unique<MaiToolRegistry>();
        // 两个移动端只提供真实可用的本地文件和网络工具，不暴露桌面 shell。
        registerMaiFileTools(*tools);
        registerMaiCurlTools(*tools, config.caBundlePath);
        tools->add(makeMaiZlibCompressTool());
        tools->add(makeMaiZlibDecompressTool());
        tools->add(makeMaiOpenSslCliTool());
        tools->add(makeMaiSystemResourcesTool([dispatcher = hostTools]() -> std::optional<MaiGpuResources> {
            const MaiToolResult response = callMaiMobileHostTool(dispatcher, "mobile_gpu_info", "{}");
            if (response.hasError()) return std::nullopt;
            const Json parsed = Json::parse(response.output(), nullptr, false);
            if (!parsed.is_object() || !parsed.value("available", false)) return std::nullopt;
            MaiGpuResources gpu;
            gpu.name = parsed.value("name", std::string{});
            gpu.unifiedMemory = parsed.value("unified_memory", false);
            if (parsed.value("app_allocated_bytes", Json{}).is_number_unsigned())
                gpu.appAllocatedBytes = parsed["app_allocated_bytes"].get<std::uint64_t>();
            if (parsed.value("recommended_working_set_bytes", Json{}).is_number_unsigned())
                gpu.recommendedWorkingSetBytes = parsed["recommended_working_set_bytes"].get<std::uint64_t>();
            return gpu;
        }));
        tools->add(makeMaiNetworkIpTool(config.caBundlePath));
        if (request.value("temporaryDirectory", Json{}).is_string() &&
            request.value("cacheDirectory", Json{}).is_string()) {
            tools->add(makeMaiAppStorageTool({request["temporaryDirectory"].get<std::string>(),
                                              request["cacheDirectory"].get<std::string>(),
                                              request.at("workspace").get<std::string>()}));
        }
        const auto sshPassword = [dispatcher = hostTools](const std::string& host, int port,
                                                          const std::string& username) -> MaiResult<std::string> {
            const MaiToolResult response = callMaiMobileHostTool(
                dispatcher, "mobile_ssh_password", Json{{"host", host}, {"port", port}, {"username", username}}.dump());
            if (response.hasError()) return response.error();
            const Json parsed = Json::parse(response.output(), nullptr, false);
            if (!parsed.is_object() || !parsed.value("password", Json{}).is_string())
                return {MaiErrorCode::Protocol, "SSH password dialog returned no password"};
            return parsed["password"].get<std::string>();
        };
        const auto sshTrust = [dispatcher = hostTools](const std::string& host, int port,
                                                       const std::string& fingerprint) -> MaiResult<bool> {
            const MaiToolResult response =
                callMaiMobileHostTool(dispatcher, "mobile_ssh_trust_host",
                                      Json{{"host", host}, {"port", port}, {"fingerprint", fingerprint}}.dump());
            if (response.hasError()) return response.error();
            const Json parsed = Json::parse(response.output(), nullptr, false);
            if (!parsed.is_object() || !parsed.value("trusted", Json{}).is_boolean())
                return {MaiErrorCode::Protocol, "SSH host-key dialog returned no decision"};
            return parsed["trusted"].get<bool>();
        };
        tools->add(makeMaiSshTool(sshPassword, sshTrust));
        tools->add(makeMaiAgentSendMediaTool());
        tools->add(makeMaiQuestionTool());
        tools->add(makeMaiCurrentTimeTool());
        tools->add(makeMaiTodoWriteTool());
        tools->add(makeMaiViewImageTool(makeMaiFfmpegImagePreview(ffmpegEngine)));
        tools->add(makeMaiFfmpegTool(ffmpegEngine));
        tools->add(makeMaiFfprobeTool(ffmpegEngine));
        tools->add(makeMaiStretchLowerVideoTool(ffmpegEngine));
        tools->add(makeMaiCvSceneDetectTool(analyzeMaiCvVideo));
        tools->add(makeMaiCvMotionDetectTool(analyzeMaiCvVideo));
        tools->add(makeMaiCvImageQualityTool(analyzeMaiCvImage));
        tools->add(makeMaiCvImageCompareTool(analyzeMaiCvImage));
        tools->add(makeMaiCvFindContoursTool(analyzeMaiCvImage));
        tools->add(makeMaiCvDetectEdgesTool(analyzeMaiCvImage));
        tools->add(makeMaiCvTemplateMatchTool(analyzeMaiCvImage));
        tools->add(makeMaiCvRegisterTranslationTool(analyzeMaiCvImage));
        tools->add(makeMaiCvDetectLinesTool(analyzeMaiCvImage));
        tools->add(makeMaiCvDocumentCornersTool(analyzeMaiCvImage));
        tools->add(makeMaiCvThresholdMaskTool(analyzeMaiCvImage));
        const auto arkKey = [dispatcher = hostTools]() -> std::string {
            const MaiToolResult result = callMaiMobileHostTool(dispatcher, "ark_api_key", "{}");
            if (result.hasError()) return {};
            const Json response = Json::parse(result.output(), nullptr, false);
            return response.is_object() && response.value("key", Json{}).is_string()
                       ? response["key"].get<std::string>()
                       : std::string{};
        };
        const auto assetSettings = [dispatcher = hostTools]() -> MaiResult<MaiArkAssetServiceSettings> {
            const MaiToolResult response = callMaiMobileHostTool(dispatcher, "mobile_ark_service_config", "{}");
            if (response.hasError()) return response.error();
            const Json value = Json::parse(response.output(), nullptr, false);
            if (!value.is_object() || !value.value("base_url", Json{}).is_string() ||
                !value.value("token", Json{}).is_string())
                return {MaiErrorCode::Protocol, "Ark Assets service configuration is invalid"};
            return MaiArkAssetServiceSettings{value["base_url"].get<std::string>(), value["token"].get<std::string>()};
        };
        // 各模型复用同一套私有 OSS 直传实现，但必须写入各自的明确目录。
        const bool assetsConfigured = static_cast<bool>(assetSettings());
        const auto modelUploadMedia = [&](MaiOssUploadPurpose purpose) {
            return assetsConfigured ? makeMaiPrivateOssMediaUploader(assetSettings, config.caBundlePath, purpose)
                                    : MaiCreativeMediaUploadProvider{};
        };
        const MaiCreativeMediaUploadProvider seedreamUploadMedia = modelUploadMedia(MaiOssUploadPurpose::SeedreamInput);
        const MaiCreativeMediaUploadProvider miniMaxUploadMedia = modelUploadMedia(MaiOssUploadPurpose::MiniMaxInput);
        const MaiCreativeMediaUploadProvider klingUploadMedia = modelUploadMedia(MaiOssUploadPurpose::KlingInput);
        const MaiCreativeMediaUploadProvider wanUploadMedia = modelUploadMedia(MaiOssUploadPurpose::WanInput);
        const MaiArkAssetProvider assetProvider =
            assetsConfigured ? makeMaiArkAssetServiceProvider(assetSettings, config.caBundlePath)
                             : MaiArkAssetProvider{};
        const MaiVideoMetadataProvider seedanceVideoMetadata = makeMaiFfprobeVideoMetadataProvider(ffmpegEngine);
        if (assetsConfigured) {
            // Seedance 本地素材由同一 C++ 资产通道完成 OSS 上传、Ark 入库与状态查询。
            // 密钥和服务令牌由宿主提供；工具拿到的是回调，不把凭据写进模型提示词。
            // 对主模型隐藏，后台仍按工具名轮询已提交的 Seedance 任务。
            tools->addInternal(
                makeMaiSeedanceVideoTool(arkKey, config.caBundlePath, assetProvider, seedanceVideoMetadata));
            tools->add(makeMaiArkAssetTool(assetProvider));
        } else {
            tools->addInternal(makeMaiSeedanceVideoTool(arkKey, config.caBundlePath, {}, seedanceVideoMetadata));
            tools->add(makeMaiArkAssetTool());
        }
        tools->addInternal(makeMaiSeedreamImageTool(arkKey, config.caBundlePath, seedreamUploadMedia));
        const auto glmKey = [dispatcher = hostTools]() -> std::string {
            const MaiToolResult result = callMaiMobileHostTool(dispatcher, "glm_api_key", "{}");
            if (result.hasError()) return {};
            const Json response = Json::parse(result.output(), nullptr, false);
            return response.is_object() && response.value("key", Json{}).is_string()
                       ? response["key"].get<std::string>()
                       : std::string{};
        };
        tools->add(makeMaiGlmImageTool(glmKey, config.caBundlePath));
        const auto miniMaxKey = [dispatcher = hostTools]() -> std::string {
            const MaiToolResult result = callMaiMobileHostTool(dispatcher, "minimax_api_key", "{}");
            if (result.hasError()) return {};
            const Json response = Json::parse(result.output(), nullptr, false);
            return response.is_object() && response.value("key", Json{}).is_string()
                       ? response["key"].get<std::string>()
                       : std::string{};
        };
        tools->add(makeMaiMiniMaxVideoTool(miniMaxKey, config.caBundlePath, miniMaxUploadMedia));
        tools->add(makeMaiMiniMaxImageTool(miniMaxKey, config.caBundlePath, miniMaxUploadMedia));
        const auto wanCredentials = [dispatcher = hostTools]() -> MaiWanCredentials {
            const MaiToolResult result = callMaiMobileHostTool(dispatcher, "wan_credentials", "{}");
            if (result.hasError()) return {};
            const Json response = Json::parse(result.output(), nullptr, false);
            if (!response.is_object()) return {};
            return {response.value("key", std::string{}), response.value("workspace_id", std::string{})};
        };
        tools->add(makeMaiWanVideoTool(wanCredentials, config.caBundlePath, wanUploadMedia));
        tools->add(makeMaiBailianKlingVideoTool(wanCredentials, config.caBundlePath, klingUploadMedia));
        tools->add(makeMaiWanImageTool(wanCredentials, config.caBundlePath, wanUploadMedia));
        tools->add(makeMaiBailianKlingImageTool(wanCredentials, config.caBundlePath, klingUploadMedia));
        const std::string rvmModel = request.value("rvmModelPath", "");
        const std::string lamaModel = request.value("lamaModelPath", "");
        const std::string ortRuntime = request.value("ortRuntimePath", "");
        if (!rvmModel.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(rvmModel))) {
            if (ortApiBase != nullptr ||
                (!ortRuntime.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(ortRuntime)))) {
                tools->add(makeMaiVideoMattingTool(maiMatteVideo, rvmModel, ortRuntime, ortApiBase));
            }
        }
        // 去水印仅暴露新的本地模型 + 临近干净帧工具，不再注册旧 delogo 实现。
        if (!lamaModel.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(lamaModel)) &&
            (ortApiBase != nullptr ||
             (!ortRuntime.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(ortRuntime))))) {
            tools->add(makeMaiTemporalWatermarkTool(maiRepairVideoWatermarks, lamaModel, ortRuntime, ortApiBase));
        }
        tools->add(makeMaiPdfTool([dispatcher = hostTools](const std::string& html, const std::string& output,
                                                           const std::atomic<bool>* cancel) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
                return MaiToolResult::failure(MaiErrorCode::Canceled, "PDF generation was canceled");
            return callMaiMobileHostTool(dispatcher, "generate_pdf",
                                         Json{{"html", html}, {"output_path", output}}.dump());
        }));
        registerMaiChatHostTools(*tools, hostTools);
        registerMobilePhotoTools(*tools, hostTools);
        MaiAgent::Options options;
        options.defaultModel = request.at("model").get<std::string>();
        options.fileAccessRoot = request.value("appRoot", request.at("workspace").get<std::string>());
        options.decodeText = [dispatcher = hostTools](const std::string& bytes,
                                                      const std::string& encoding) -> MaiResult<std::string> {
            const MaiToolResult result = callMaiMobileHostTool(
                dispatcher, "mobile_decode_text", Json{{"base64", encodeBase64(bytes)}, {"encoding", encoding}}.dump());
            if (result.hasError()) return result.error();
            return result.output();
        };
        options.baseInstructions = kMarkdownBaseInstructions;
        const auto policy = request.value("policy", "on-request");
        if (policy == "never")
            options.approvalPolicy = MaiApprovalPolicy::Never;
        else if (policy == "unless-trusted")
            options.approvalPolicy = MaiApprovalPolicy::UnlessTrusted;
        else if (policy != "on-request")
            throw std::runtime_error("Unknown approval policy.");
        auto replacement =
            std::make_unique<MaiAgent>(std::move(store.value()), makeMaiModelClient(config), std::move(tools), options);
        agent.reset();
        pendingDeltas.clear();
        liveParts.clear();
        errors.clear();
        agent = std::move(replacement);
        workspace = request.at("workspace").get<std::string>();
        model = options.defaultModel;
        configured = !config.apiKey.empty() && !model.empty();
        dirty = true;
        structureChanged = true;
        cachedSession.clear();
        cachedLimit = -1;
        cachedMessages.clear();
        agent->eventBus().subscribe([this](const MaiEvent& event) {
            // 网络线程只合并增量，不读数据库、不解析 Markdown、不调用 UI。
            std::lock_guard<std::mutex> lock(mutex);
            dirty = true;
            if (event.type == MaiEventType::MessagePartDelta)
                pendingDeltas[event.partId] += event.delta;
            else
                structureChanged = true;
            if (event.type == MaiEventType::SessionError) errors[event.sessionId] = event.detail;
        });
    }

    // 将持久化消息转为移动端可读快照，并保留最新消息和未完成任务的状态。
    // 返回值只包含本次请求选中的会话内容，不改动 Agent 存储。
    Json serializeMessages(const std::vector<MaiMessage>& source, bool sessionBusy) {
        Json messages = Json::array();
        std::string latestId;
        for (const auto& message : source)
            if (message.id > latestId) latestId = message.id;
        for (const auto& message : source) {
            Json parts = Json::array();
            for (const auto& part : message.parts) {
                Json value = {{"id", part.id}};
                std::visit(
                    [&](const auto& body) {
                        using T = std::decay_t<decltype(body)>;
                        if constexpr (std::is_same_v<T, MaiToolPart>) {
                            value.update({{"kind", "tool"},
                                          {"tool", body.tool},
                                          {"input", body.input},
                                          {"output", body.output},
                                          {"error", body.error},
                                          {"state", maiToolStateToString(body.state)}});
                        } else if constexpr (std::is_same_v<T, MaiImagePart>) {
                            value.update({{"kind", "image"}, {"path", body.path}, {"mimeType", body.mimeType}});
                        } else if constexpr (std::is_same_v<T, MaiVideoPart>) {
                            value.update({{"kind", "video"}, {"path", body.path}, {"mimeType", body.mimeType}});
                        } else if constexpr (std::is_same_v<T, MaiQuotePart>) {
                            value.update({{"kind", "quote"}, {"messageId", body.messageId}, {"preview", body.preview}});
                        } else {
                            std::string text = body.text;
                            auto live = liveParts.find(part.id);
                            if (message.isInProgress() && live != liveParts.end() && live->second.size() > text.size())
                                text = live->second;
                            value.update(
                                {{"kind", std::is_same_v<T, MaiTextPart> ? "text" : "reasoning"}, {"text", text}});
                        }
                    },
                    part.body);
                parts.push_back(std::move(value));
                if (!message.isInProgress()) liveParts.erase(part.id);
            }
            messages.push_back({{"id", message.id},
                                {"role", maiRoleToString(message.role)},
                                {"created", message.created},
                                {"completed", message.completed},
                                {"active", message.isInProgress() && sessionBusy && message.id == latestId},
                                {"parts", parts}});
        }
        return messages;
    }

    // 作用：读取指定会话的有限消息快照和任务状态。
    Json snapshot(const std::string& selected, bool force, int limit) {
        std::unordered_map<std::string, std::string> errorCopy;
        std::unordered_set<std::string> changedParts;
        bool reload = force || cachedSession != selected || cachedLimit != limit;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!dirty && !force) return {{"ok", true}, {"changed", false}};
            dirty = false;
            reload |= structureChanged;
            structureChanged = false;
            for (auto& delta : pendingDeltas) {
                liveParts[delta.first] += delta.second;
                changedParts.insert(delta.first);
            }
            pendingDeltas.clear();
            errorCopy = errors;
        }
        Json sessions = Json::array();
        bool busy = false;
        for (const auto& session : agent->listSessions()) {
            if (!session.isRoot()) continue;
            const bool active = agent->isBusy(session.id);
            busy |= active;
            sessions.push_back({{"id", session.id}, {"title", session.title}, {"busy", active}});
        }
        // 增量期间复用消息结构，避免每个批次重新读整段 SQLite 历史。
        if (reload) {
            if (limit < 0)
                cachedMessages = agent->listMessages(selected);
            else if (limit == 0)
                cachedMessages.clear();
            else
                cachedMessages = agent->listMessagesPage(selected, "", limit);
            cachedSession = selected;
            cachedLimit = limit;
        }
        const bool sessionBusy = agent->isBusy(selected);
        Json messages;
        if (limit >= 0 && !reload && !changedParts.empty()) {
            std::vector<MaiMessage> changedMessages;
            for (const auto& message : cachedMessages) {
                for (const auto& part : message.parts) {
                    if (changedParts.count(part.id) == 0) continue;
                    changedMessages.push_back(message);
                    break;
                }
            }
            messages = serializeMessages(changedMessages, sessionBusy);
        } else {
            messages = serializeMessages(cachedMessages, sessionBusy);
        }
        Json permissions = Json::array(), questions = Json::array();
        for (const auto& p : agent->listPendingPermissions())
            if (p.sessionId == selected)
                permissions.push_back({{"id", p.id},
                                       {"tool", p.toolName},
                                       {"input", p.arguments},
                                       {"allowForSession", p.allowForSession},
                                       {"rememberOnApproval", p.rememberOnApproval},
                                       {"fileCount", p.approvalKeys.size()}});
        for (const auto& q : agent->listPendingQuestions())
            if (q.sessionId == selected)
                questions.push_back({{"id", q.id}, {"question", q.question}, {"options", q.options}});
        if (!busy) liveParts.clear();
        return {{"ok", true},
                {"changed", true},
                {"sessions", sessions},
                {"messages", messages},
                {"permissions", permissions},
                {"questions", questions},
                {"busy", busy},
                {"configured", configured},
                {"error", errorCopy[selected]}};
    }

    Json request(const Json& r) {
        const std::string op = r.at("op");
        if (op == "configure") {
            configure(r);
            return {{"ok", true}};
        }
        if (!agent) throw std::runtime_error("Agent is not initialized.");
        const std::string session = r.value("session", "");
        if (op == "snapshot") {
            const int limit = r.value("messageLimit", -1);
            if (limit < -1 || limit > 100) throw std::runtime_error("messageLimit must be between 0 and 100.");
            return snapshot(session, r.value("force", false), limit);
        }
        if (op == "messages_page") {
            const int limit = r.value("limit", 30);
            if (limit < 1 || limit > 100) throw std::runtime_error("limit must be between 1 and 100.");
            const std::string before = r.value("before", "");
            const auto page = agent->listMessagesPage(session, before, limit);
            return {{"ok", true}, {"messages", serializeMessages(page, before.empty() && agent->isBusy(session))}};
        }
        if (op == "suggest_replies") {
            if (!configured) throw std::runtime_error("Configure a model and API key first.");
            if (!r.contains("messages") || !r["messages"].is_array() || r["messages"].empty() ||
                r["messages"].size() > 30)
                throw std::runtime_error("Messages must contain between 1 and 30 items.");
            MaiModelRequest modelRequest;
            modelRequest.model = model;
            modelRequest.temperature = 0.5;
            modelRequest.baseInstructions =
                "You generate reply suggestions for a private chat. Return only one "
                "JSON object "
                "with exactly these string fields: natural, casual, professional. "
                "Each value must "
                "be a short reply in the conversation language. Do not send a "
                "message, call a "
                "tool, use Markdown fences, or include explanations.";
            MaiModelMessage context;
            context.role = MaiModelRole::User;
            context.content = "Recent conversation, oldest first:\n" + r["messages"].dump();
            modelRequest.messages.push_back(std::move(context));
            std::string response;
            MaiStreamSink sink;
            sink.onText = [&](std::string_view delta) { response.append(delta.data(), delta.size()); };
            std::atomic<bool> cancel{false};
            const MaiError error = makeMaiModelClient(suggestionConfig)->stream(modelRequest, sink, cancel);
            if (error) throw std::runtime_error(error.message());
            std::string clean = response;
            const auto firstLine = clean.find('\n');
            const auto closingFence = clean.rfind("```");
            if (clean.rfind("```", 0) == 0 && firstLine != std::string::npos && closingFence > firstLine)
                clean = clean.substr(firstLine + 1, closingFence - firstLine - 1);
            const Json suggestions = Json::parse(clean);
            for (const char* key : {"natural", "casual", "professional"})
                if (!suggestions.contains(key) || !suggestions[key].is_string() ||
                    suggestions[key].get<std::string>().empty())
                    throw std::runtime_error("The model returned incomplete reply suggestions.");
            return {{"ok", true},
                    {"natural", suggestions["natural"]},
                    {"casual", suggestions["casual"]},
                    {"professional", suggestions["professional"]}};
        }
        std::string id;
        if (op == "create")
            id = result(agent->submit(MaiCreateSession{workspace, "", model}));
        else if (op == "send") {
            if (!configured) throw std::runtime_error("Configure a model and API key first.");
            // iOS 在升级或恢复后可能给同一数据容器分配新的绝对路径。图片片段只保存
            // 工作区相对路径，所以每轮发送前都把会话根目录迁到当前容器位置。
            result(agent->submit(MaiUpdateSession{session, "", model, "", workspace}));
            {
                std::lock_guard<std::mutex> lock(mutex);
                errors.erase(session);
            }
            std::vector<MaiModelImage> images;
            if (r.contains("images")) {
                if (!r["images"].is_array() || r["images"].size() > 10)
                    throw std::runtime_error("Images must be an array with at most 10 items.");
                for (const auto& value : r["images"]) {
                    if (!value.is_object() || !value.contains("path") || !value["path"].is_string() ||
                        !value.contains("mimeType") || !value["mimeType"].is_string())
                        throw std::runtime_error("Each image needs path and mimeType strings.");
                    images.push_back({value["path"].get<std::string>(), value["mimeType"].get<std::string>()});
                }
            }
            std::vector<MaiVideoPart> videos;
            if (r.contains("videos")) {
                if (!r["videos"].is_array() || r["videos"].size() > 10)
                    throw std::runtime_error("Videos must be an array with at most 10 items.");
                for (const auto& value : r["videos"]) {
                    if (!value.is_object() || !value.contains("path") || !value["path"].is_string() ||
                        !value.contains("mimeType") || !value["mimeType"].is_string())
                        throw std::runtime_error("Each video needs path and mimeType strings.");
                    videos.push_back({value["path"].get<std::string>(), value["mimeType"].get<std::string>()});
                }
            }
            if (r.contains("quoteMessageId") && !r["quoteMessageId"].is_string())
                throw std::runtime_error("quoteMessageId must be a string.");
            id = result(agent->submit(MaiSendPrompt{session, r.at("text"), std::move(images), std::move(videos),
                                                    r.value("quoteMessageId", std::string{})}));
        } else if (op == "stop")
            result(agent->submit(MaiInterrupt{session}));
        else if (op == "delete")
            result(agent->submit(MaiDeleteSession{session}));
        else if (op == "clear")
            result(agent->submit(MaiClearMessages{session}));
        else if (op == "rename")
            result(agent->submit(MaiUpdateSession{session, r.at("title"), "", ""}));
        else if (op == "permission") {
            MaiPermissionDecision decision = MaiPermissionDecision::Denied;
            if (!maiParsePermissionDecision(r.at("decision"), decision))
                throw std::runtime_error("Unknown permission decision.");
            result(agent->submit(MaiReplyPermission{r.at("id"), decision}));
        } else if (op == "answer")
            result(agent->submit(MaiReplyQuestion{r.at("id"), r.at("text")}));
        else
            throw std::runtime_error("Unknown operation.");
        return {{"ok", true}, {"id", id}};
    }
};

void* maiMobileAgentCreate(void) {
    try {
        return new MaiMobileAgent();
    } catch (...) {
        return nullptr;
    }
}
void maiMobileAgentDestroy(void* handle) {
    delete static_cast<MaiMobileAgent*>(handle);
}
int maiMobileAgentSetHostToolHandler(void* handle, void* context, MaiMobileHostToolHandler handler,
                                     MaiMobileHostToolResponseFree responseFree,
                                     MaiMobileHostToolContextRelease contextRelease) {
    if (handle == nullptr) return 0;
    auto* mobile = static_cast<MaiMobileAgent*>(handle);
    if (mobile->anyBusy()) return 0;
    return setMaiMobileHostToolHandler(mobile->hostTools, context, handler, responseFree, contextRelease) ? 1 : 0;
}
int maiMobileAgentSetOrtApiBase(void* handle, const void* apiBase) {
    if (handle == nullptr || apiBase == nullptr) return 0;
    auto* mobile = static_cast<MaiMobileAgent*>(handle);
    if (mobile->agent != nullptr) return 0;
    mobile->ortApiBase = apiBase;
    return 1;
}
char* maiMobileMatteVideo(const char* argumentsJson, const char* workspace, const char* modelPath,
                          const char* runtimePath, const void* apiBase) {
    try {
        if (argumentsJson == nullptr || workspace == nullptr || modelPath == nullptr)
            return ownedResponse(R"({"ok":false,"error":"invalid matting arguments"})");
        auto tool =
            makeMaiVideoMattingTool(maiMatteVideo, modelPath, runtimePath != nullptr ? runtimePath : "", apiBase);
        if (!tool) return ownedResponse(R"({"ok":false,"error":"video matting is unavailable"})");
        MaiToolContext context;
        context.root = workspace;
        context.allowOutsideWorkingDirectory = true;
        const MaiToolResult result = tool->execute(argumentsJson, context);
        if (result.hasError())
            return ownedResponse(Json{{"ok", false},
                                      {"errorCode", maiErrorCodeToString(result.error().code())},
                                      {"error", result.error().message()}}
                                     .dump());
        return ownedResponse(Json{{"ok", true}, {"output", result.output()}}.dump());
    } catch (const std::exception& error) {
        return ownedResponse(Json{{"ok", false}, {"error", error.what()}}.dump());
    }
}
char* maiMobileAgentRequest(void* handle, const char* request) {
    std::string response;
    try {
        if (!handle || !request) throw std::runtime_error("Invalid bridge handle.");
        response = static_cast<MaiMobileAgent*>(handle)->request(Json::parse(request)).dump();
    } catch (const std::exception& e) {
        response = Json{{"ok", false}, {"error", e.what()}}.dump();
    } catch (...) {
        response = "{\"ok\":false,\"error\":\"Native agent failure.\"}";
    }
    return ownedResponse(response);
}
void maiMobileAgentFree(char* response) {
    std::free(response);
}
