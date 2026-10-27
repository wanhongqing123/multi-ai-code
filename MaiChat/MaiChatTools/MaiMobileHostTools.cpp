// 文件作用：把 Agent 工具调用转发给移动端宿主，并管理处理器注册、并发调用与释放。
// 调用边界：由 MaiChat 媒体或 Agent 宿主调用；平台资源在停止或失败时须正确释放。
#include "MaiMobileHostTools.h"

#include <cstdlib>
#include <cstring>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <string>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"

using Json = nlohmann::json;

class MaiMobileHostDispatcher {
public:
    ~MaiMobileHostDispatcher() {
        clear();
    }

    bool set(void* context, MaiMobileHostToolHandler handler,
             MaiMobileHostToolResponseFree responseFree,
             MaiMobileHostToolContextRelease contextRelease) {
        if (handler == nullptr && context != nullptr) return false;
        if ((handler == nullptr) != (responseFree == nullptr) ||
            (handler == nullptr) != (contextRelease == nullptr))
            return false;
        void* previousContext = nullptr;
        MaiMobileHostToolContextRelease previousRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mActiveCalls != 0) return false;
            previousContext = mContext;
            previousRelease = mContextRelease;
            mContext = context;
            mHandler = handler;
            mResponseFree = responseFree;
            mContextRelease = contextRelease;
        }
        if (previousRelease != nullptr) previousRelease(previousContext);
        return true;
    }

    // 先在锁内固定宿主回调和上下文，再解锁执行；宿主可能回调 Agent，不能持锁跨越边界。
    // 归还宿主响应后统一解析结果，并把协议错误映射为 Agent 可识别的失败类型。
    MaiToolResult call(const std::string& toolName, const std::string& argumentsJson) {
        void* context = nullptr;
        MaiMobileHostToolHandler handler = nullptr;
        MaiMobileHostToolResponseFree responseFree = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mHandler == nullptr)
                return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                              "the MaiChat host tool handler is not registered");
            ++mActiveCalls;
            context = mContext;
            handler = mHandler;
            responseFree = mResponseFree;
        }

        const char* response = handler(context, toolName.c_str(), argumentsJson.c_str());
        std::string responseJson;
        if (response != nullptr) responseJson = response;
        if (response != nullptr) responseFree(context, response);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            --mActiveCalls;
        }
        if (responseJson.empty())
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "the MaiChat host returned an empty response");
        try {
            const Json parsed = Json::parse(responseJson);
            const bool success = parsed.value("ok", parsed.value("success", false));
            if (!success) {
                const std::string code = parsed.value("errorCode", "internal");
                MaiErrorCode errorCode = MaiErrorCode::Internal;
                if (code == "invalid_input") errorCode = MaiErrorCode::InvalidInput;
                else if (code == "not_found") errorCode = MaiErrorCode::NotFound;
                else if (code == "not_configured") errorCode = MaiErrorCode::NotConfigured;
                else if (code == "canceled") errorCode = MaiErrorCode::Canceled;
                else if (code == "network") errorCode = MaiErrorCode::Network;
                else if (code == "protocol") errorCode = MaiErrorCode::Protocol;
                return MaiToolResult::failure(
                    errorCode, parsed.value("error", parsed.value(
                        "errorMessage", "the MaiChat host tool failed")));
            }
            if (!parsed.contains("output"))
                return MaiToolResult::failure(MaiErrorCode::Protocol,
                                              "the MaiChat host response has no output");
            return MaiToolResult::success(parsed["output"].is_string()
                                              ? parsed["output"].get<std::string>()
                                              : parsed["output"].dump());
        } catch (const std::exception& error) {
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          std::string("invalid MaiChat host response: ") +
                                              error.what());
        }
    }

    void clear() {
        void* context = nullptr;
        MaiMobileHostToolContextRelease contextRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mActiveCalls != 0) return;
            context = mContext;
            contextRelease = mContextRelease;
            mContext = nullptr;
            mHandler = nullptr;
            mResponseFree = nullptr;
            mContextRelease = nullptr;
        }
        if (contextRelease != nullptr) contextRelease(context);
    }

private:
    std::mutex mMutex;
    void* mContext = nullptr;
    MaiMobileHostToolHandler mHandler = nullptr;
    MaiMobileHostToolResponseFree mResponseFree = nullptr;
    MaiMobileHostToolContextRelease mContextRelease = nullptr;
    int mActiveCalls = 0;
};

namespace {

class MaiMobileHostTool final : public MaiTool {
public:
    MaiMobileHostTool(std::string name, std::string description, std::string schema,
                      std::shared_ptr<MaiMobileHostDispatcher> dispatcher, bool approval)
        : mName(std::move(name)),
          mDescription(std::move(description)),
          mSchema(std::move(schema)),
          mDispatcher(std::move(dispatcher)),
          mApproval(approval) {}

    std::string name() const override { return mName; }
    std::string description() const override { return mDescription; }
    std::string parametersSchema() const override { return mSchema; }
    bool requiresApproval(const std::string&) const override { return mApproval; }
    bool requiresPerCallApproval(const std::string&) const override { return mApproval; }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext&) override {
        return mDispatcher->call(mName, argumentsJson);
    }

private:
    std::string mName;
    std::string mDescription;
    std::string mSchema;
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
    bool mApproval = false;
};

class MaiMobileScreenCaptureTool final : public MaiTool {
public:
    explicit MaiMobileScreenCaptureTool(std::shared_ptr<MaiMobileHostDispatcher> dispatcher)
        : mDispatcher(std::move(dispatcher)) {}

    std::string name() const override { return "mobile_capture_screen"; }
    std::string description() const override {
        return "Read the most recent whole-device screen frame from a screen-sharing session "
               "that the user started with mobile_start_screen_share. This can include other "
               "apps. If sharing is not active or there is no recent frame, ask the user to "
               "start sharing and visit the target app first.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{},"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override { return false; }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const Json arguments = Json::parse(argumentsJson, nullptr, false);
        if (!arguments.is_object() || !arguments.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "mobile_capture_screen expects an empty object");
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
        if (context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "the Agent workspace is not configured");
        const MaiFilePath directory = MaiFilePath::fromUtf8(context.root)
                                          .append(MaiFilePath::fromUtf8("screenshots"));
        const MaiError directoryError = MaiFileSystem::createDirectories(directory);
        if (directoryError.hasError())
            return MaiToolResult::failure(directoryError.code(), directoryError.message());
        const MaiFilePath path = directory.append(MaiFilePath::fromUtf8(
            MaiIdGenerator::generate("mobile_screen_") + ".png"));
        const MaiToolResult response = callMaiMobileHostTool(
            mDispatcher, name(), Json{{"output_path", path.toUtf8()}}.dump());
        if (response.hasError()) return response;
        if (!MaiFileSystem::exists(path))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "screen capture did not write a PNG file");
        return MaiToolResult::successWithImages(response.output(),
                                                {MaiToolImage{path.toUtf8(), "image/png"}});
    }

private:
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
};

}  // namespace

std::shared_ptr<MaiMobileHostDispatcher> makeMaiMobileHostDispatcher() {
    return std::make_shared<MaiMobileHostDispatcher>();
}

bool setMaiMobileHostToolHandler(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                 void* context, MaiMobileHostToolHandler handler,
                                 MaiMobileHostToolResponseFree responseFree,
                                 MaiMobileHostToolContextRelease contextRelease) {
    return dispatcher->set(context, handler, responseFree, contextRelease);
}

void clearMaiMobileHostToolHandler(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    dispatcher->clear();
}

MaiToolResult callMaiMobileHostTool(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                   const std::string& name, const std::string& argumentsJson) {
    return dispatcher->call(name, argumentsJson);
}

void addMaiMobileHostTool(MaiToolRegistry& tools, const char* name, const char* description,
                          const char* schema,
                          const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                          bool approval) {
    tools.add(std::make_unique<MaiMobileHostTool>(name, description, schema, dispatcher, approval));
}

// 集中声明聊天、诊断和媒体宿主工具，保持名称、入参 Schema 与宿主处理器一一对应。
// 具有外部副作用的入口要求审批，注册时不能因同组只读工具而放宽权限。
void registerMaiChatHostTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    const auto add = [&](const char* name, const char* description, const char* schema,
                         bool approval = false) {
        tools.add(std::make_unique<MaiMobileHostTool>(name, description, schema, dispatcher,
                                                      approval));
    };
    // MaiChat 内部联系人与手机系统通讯录是两套数据：以下 maichat_* 只查 IM 会话。
    add("maichat_list_contacts",
        "List MaiChat contacts. Use query to filter by user ID or display name.",
        // query 按用户 ID 或显示名筛选；limit 限本次最多返回 200 人。
        R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,)"
            R"("maximum":200}}})");
    add("maichat_list_groups",
        "List joined MaiChat group conversations. Use query to filter by group ID or name.",
        R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,)"
            R"("maximum":200}},"additionalProperties":false})");
    add("maichat_create_group",
        "Create a MaiChat text group with 1 to 19 accepted friends. The owner is added "
        "automatically. Show the complete group name and member list for approval on every call.",
        R"({"type":"object","properties":{"name":{"type":"string","minLength":1},)"
            R"("member_ids":{"type":"array","items":{"type":"string"},"minItems":1,)"
            R"("maxItems":19,"uniqueItems":true}},"required":["name","member_ids"],)"
            R"("additionalProperties":false})",
        true);
    add("maichat_get_group_messages",
        "Read recent text messages from a joined MaiChat group without changing the open chat.",
        R"({"type":"object","properties":{"group_id":{"type":"string"},)"
            R"("limit":{"type":"integer","minimum":1,"maximum":20}},)"
            R"("required":["group_id"],"additionalProperties":false})");
    add("maichat_request_friend",
        "Send a MaiChat friend request by phone number or account ID. Requires user approval. "
        "The recipient must accept.",
        R"({"type":"object","properties":{"account":{"type":"string"}},"required":["account"]})",
        true);
    add("maichat_set_nickname",
        "Set or clear the current MaiChat account nickname. Empty string clears it. "
        "Requires user approval.",
        R"({"type":"object","properties":{"nickname":{"type":"string","maxLength":32}},)"
            R"("required":["nickname"]})",
        true);
    add("maichat_list_conversations",
        "List direct MaiChat conversations with contacts, ordered by latest message and unread "
        "count. Use maichat_list_groups for group conversations.",
        // 会话按最新消息排序；limit 只限制返回条数，未读数来自会话记录。
        R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":200}}})");
    add("maichat_get_messages",
        "Read recent messages in a MaiChat conversation with a specific contact.",
        // peer_id 必须是指定联系人；limit 限这一次读取的最近消息条数。
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"limit":{"type":"integer","minimum":1,)"
            R"("maximum":200}},"required":["peer_id"]})");
    add("maichat_search_messages",
        "Search MaiChat message text across all contacts or within one contact.",
        // query 是必需的检索词；peer_id 可选，传入后只查该联系人。
        R"({"type":"object","properties":{"query":{"type":"string"},"peer_id":{"type":"string"},)"
            R"("limit":{"type":"integer","minimum":1,"maximum":200}},"required":["query"]})");
    add("maichat_get_unread_summary", "Summarize unread MaiChat messages by contact.",
        // 不收参数，按联系人汇总未读消息。
        R"({"type":"object","properties":{}})");
    add("maichat_remote_session_list",
        "List Sessions on other online MaiChat devices, including devices with no Sessions. "
        "Use this to find the destination device slot before creating or deleting a Session.",
        R"({"type":"object","properties":{},"additionalProperties":false})");
    add("maichat_remote_session_create",
        "Create a Session on another online MaiChat device. Omit workspace_path to use that "
        "device's default Workspace; on Desktop an explicit path must already exist there.",
        R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
            R"("workspace_path":{"type":"string","maxLength":4096}},)"
            R"("required":["device_slot"],"additionalProperties":false})");
    add("maichat_remote_session_delete",
        "Permanently delete a Session and its chat history on another online MaiChat device. "
        "Requires approval for every call.",
        R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
            R"("session_id":{"type":"string"}},"required":["device_slot","session_id"],)"
            R"("additionalProperties":false})",
        true);
    add("maichat_remote_session_set_workspace",
        "Set an existing Session's Workspace on another online Desktop device. The path must "
        "be absolute and already exist on that device. Requires approval for every call.",
        R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
            R"("session_id":{"type":"string"},"workspace_path":{"type":"string","maxLength":4096}},)"
            R"("required":["device_slot","session_id","workspace_path"],)"
            R"("additionalProperties":false})",
        true);
    // 用户明确要求排障时才导出；只返回工作区 TXT 路径，日志正文不回灌给模型，
    // 也不自动发给联系人。AI 会话据此显示可分享的文件卡片。
    add("mobile_export_diagnostics",
        "Export this device's privacy-filtered MaiChat diagnostic log as a new TXT file in "
        "the Agent workspace. Call only when the user asks for logs or troubleshooting. "
        "Return only the file path and size, never the log contents. A shareable file card "
        "appears in the current AI conversation; this tool does not send it to anyone.",
        R"({"type":"object","properties":{},"additionalProperties":false})");
    // 发消息会影响其他人，文本、媒体、回复和群发均需要用户确认；群发每次都要确认完整名单。
    add("maichat_send_text",
        "Send text to a MaiChat contact through the host. Requires user approval.",
        // peer_id 与 text 均必填；真正发送前弹出用户确认。
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id",)"
            R"("text"]})",
        true);
    add("maichat_send_group_text",
        "Send text to a joined MaiChat group. Show the group and full text for approval "
        "on every call.",
        R"({"type":"object","properties":{"group_id":{"type":"string"},)"
            R"("text":{"type":"string","minLength":1}},"required":["group_id","text"],)"
            R"("additionalProperties":false})",
        true);
    add("maichat_send_media",
        "Send an accessible Agent workspace image or video as a persistent media bubble, "
        "or audio as a file card. The optional caption stays in the same media message. "
        "Requires user approval.",
        // file_path 指 Agent 可访问媒体；type 为 image/video/audio；caption 另发文本。
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"file_path":{"type":"string"},)"
            R"("type":{"type":"string","enum":["image","video","audio"]},"caption":{"type":"string"}},)"
            R"("required":["peer_id","file_path","type"],"additionalProperties":false})",
        true);
    add("maichat_reply_message",
        "Send a quoted reply to a MaiChat message through the host. Requires user approval.",
        // 回复要同时给联系人、原消息 ID 和回复正文；发出前逐次确认。
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"message_id":{"type":"string"},)"
            R"("text":{"type":"string"}},"required":["peer_id","message_id","text"]})",
        true);
    add("maichat_broadcast_text",
        "Send the same text to multiple MaiChat contacts through the host. The complete recipient list and text "
        "require user approval for every call.",
        // peer_ids 为本次完整收件人列表，1–200 人；群发每次都显示名单和正文供确认。
        R"({"type":"object","properties":{"peer_ids":{"type":"array","items":{"type":"string"},"minItems":1,)"
        R"("maxItems":200},"text":{"type":"string"}},"required":["peer_ids","text"]})",
        true);
#if defined(__ANDROID__)
    // 电话号码和短信能力只在 Android 注册，iOS 没有等价的系统读取接口。
    // permission 只允许列出的系统能力；Android 比 iOS 多号码和短信两项。
    constexpr const char* permissionSchema =
        R"({"type":"object","properties":{"permission":{"type":"string","enum":["photos","camera","microphone",)"
            R"("location","contacts","phone_call","calendar","notifications",)"
            R"("phone_number","sms"]}},"required":["permission"],)"
            R"("additionalProperties":false})";
#else
    constexpr const char* permissionSchema =
        R"({"type":"object","properties":{"permission":{"type":"string","enum":["photos","camera","microphone",)"
            R"("location","contacts","calendar","notifications"]}},"required":["permission"],)"
            R"("additionalProperties":false})";
#endif
    // 先按任务所需申请单项 OS 权限；有限授权可继续读取被用户选中的媒体或联系人。
    add("mobile_request_permission",
        "Ask the operating system for a MaiChat permission when a task needs it. "
        "Use photos before reading the system gallery. Ask only for the capability needed by "
        "the current task. Returns status, whether the system prompted, whether Settings is "
        "required, and the access level (including approximate or precise location). A limited "
        "photo grant can still expose selected items; write-only calendar access can create "
        "events. Location is foreground-only.",
        permissionSchema);
#if defined(__ANDROID__)
    // Android 号码可能由运营商留空，读取结果不能当作已验证身份。
    add("mobile_get_phone_number",
        "Read the Android device's default subscription phone number after phone_number permission "
        "is granted. The carrier may provide no number, and a returned number is not verified.",
        // 无其他参数；先调用 mobile_request_permission(phone_number)。
        R"({"type":"object","properties":{},"additionalProperties":false})");
    // 短信属于敏感数据：仅在用户明确要求且授权后读取，不为普通任务遍历短信。
    add("mobile_read_sms",
        "Read up to 20 recent Android SMS inbox messages only when the user specifically asks, "
        "after sms permission is granted. READ_SMS is restricted by Android and may be unavailable "
        "depending on the installer. Do not use for OTP retrieval without an explicit user request.",
        // limit 最多 20 条；after_ms 只取该时间之后的消息。
        // 读取需要用户明确请求和系统 sms 授权，不能把短信作为普通上下文抓取。
        R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":20},)"
            R"("after_ms":{"type":"integer","minimum":0}},"additionalProperties":false})");
#endif
    // 定位工具只在前台读一次当前位置；没有权限先调用 mobile_request_permission。
    add("mobile_get_location",
        "Read one current foreground location after location permission is granted. "
        "Returns latitude, longitude, horizontal accuracy, optional altitude, timestamp, "
        "and source. If permission is missing, call mobile_request_permission first.",
        // 无参数；权限和前台状态由宿主核对，返回经纬度、精度与时间戳。
        R"({"type":"object","properties":{},"additionalProperties":false})");
    // 系统通讯录工具是只读的：先授权，再分页搜索，最后按返回的 id 取单人详情。
    // iOS 有限授权时只能看到用户选中的联系人，不能把空结果解释为通讯录不存在。
    add("mobile_list_contacts",
        "List or search the phone's system contacts visible to MaiChat after contacts permission. "
        "This is different from maichat_list_contacts. Returns names, phone numbers, and email "
        "addresses in pages of at most 50; query matches these fields. A limited iOS grant "
        "exposes only the selected contacts. Call mobile_request_permission first.",
        // query 可匹配姓名、号码、邮箱；offset/limit 分页，单页最多 50 人。
        R"({"type":"object","properties":{"query":{"type":"string"},"offset":{"type":"integer","minimum":0,)"
            R"("maximum":1000},"limit":{"type":"integer","minimum":1,"maximum":50}},"additionalProperties":false})");
    add("mobile_get_contact",
        "Read one accessible system contact by the id returned from mobile_list_contacts. "
        "Requires contacts permission and returns only name, phone numbers, and email addresses.",
        // id 来自 mobile_list_contacts，不接受随意构造的联系人字段组合。
        R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"],"additionalProperties":false})");
#if defined(__ANDROID__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
    // Android 拨号需单独授予 CALL_PHONE；iOS 仍由系统确认，返回值不代表已接通。
    add("mobile_dial_phone",
#if defined(__ANDROID__)
        "Place a regular phone call on Android only when the user explicitly asks. "
        "For a named contact, use the contacts tools to find its number first. "
        "If a contact has multiple phone numbers, ask which one to use. "
        "The first call may request Android's CALL_PHONE permission; later calls reuse the grant "
        "without another MaiChat approval. A call_requested status does not prove connection.",
#else
        "Open iOS Phone confirmation for one number only when the user explicitly asks to call. "
        "The user confirms the call in iOS. Opening the confirmation does not mean the call was placed.",
#endif
        R"({"type":"object","properties":{"phone_number":{"type":"string","minLength":3,)"
            R"("maxLength":32}},"required":["phone_number"],"additionalProperties":false})",
        false);
#endif
#if defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
    add("mobile_compose_sms",
        "Open the iOS Messages composer with recipients and body only when the user explicitly "
        "asks to send an SMS. The user reviews and taps Send in the system composer. "
        "A returned composer_opened status never means the message was sent or delivered.",
        R"({"type":"object","properties":{"recipients":{"type":"array","items":{"type":"string"},)"
            R"("minItems":1,"maxItems":10},"body":{"type":"string","minLength":1,)"
            R"("maxLength":2000}},"required":["recipients","body"],"additionalProperties":false})",
        false);
#endif
    // 会话内视频预览和 IM 视频消息走同一个播放器；控制指令只操作当前打开的实例。
    // 创作中心打开视频时播放器固定在上半屏，下半屏继续输入自然语言控制当前视频。
    add("maichat_play_video",
        "Open a video from the Agent workspace in MaiChat's FFplay/Graphics player. "
        "The player occupies the upper part of the screen while chat stays usable below, so "
        "the user can ask for playback changes while it runs. Set start_seconds to begin at a "
        "specific source-media time, instead of opening then immediately seeking. "
        "IM video messages use the same player.",
        // path 是当前 Agent 可访问的现有视频；start_seconds 是可选的原片绝对起播秒数。
        // 直接从指定位置打开比“先打开、立刻再调用 seek”更稳定。
        R"({"type":"object","properties":{"path":{"type":"string"},"start_seconds":{"type":"number","minimum":0}},)"
            R"("required":["path"],"additionalProperties":false})");
    add("maichat_video_command",
        "Control the currently open FFplay video. set_speed accepts 0.25, 0.5, 1, 1.5, or 2 "
        "and changes audio tempo without changing pitch using SoundTouch. It briefly restarts the same "
        "player at the source position without creating a new file. If the user asks to jump to "
        "a specific time such as 8 seconds, call seek_seconds with seconds=8 on the currently "
        "open video; do not reopen it. For 'replay', 'restart', or 'play from the beginning', "
        "use replay, which restarts the current video at 0 seconds with audio. "
        "seek_seconds jumps to an "
        "absolute source-media time; existing play, pause, step, relative/chapter/percent seek, "
        "stream/filter cycling, mute, volume, fullscreen, and close remain available.",
        // 用户说“跳到第 8 秒”就对当前播放实例调用 seek_seconds(seconds=8)，无需重新打开。
        // replay 在当前播放器中从 0 秒重新播放；seek_seconds(0) 也按重播处理。
        // set_speed 带 speed 倍数；seek_seconds 带源片绝对秒数；seek_percent 带百分比。
        // 改速仍在现有播放器中播放同一文件；SoundTouch.setTempo 保持音调，不另存转码文件。
        R"({"type":"object","properties":{"action":{"type":"string","enum":["play","pause","toggle_pause","replay",)"
            R"("step","seek_forward","seek_backward","seek_minute_forward","seek_minute_backward","next_chapter",)"
            R"("previous_chapter","seek_percent","seek_seconds","set_speed","next_audio","next_video","next_subtitle",)"
            R"("next_program","next_filter","mute","volume_up","volume_down","fullscreen","close"]},)"
            R"("percent":{"type":"number","minimum":0,"maximum":100},"seconds":{"type":"number","minimum":0},)"
            R"("speed":{"type":"number","enum":[0.25,0.5,1,1.5,2]}},"required":["action"],"additionalProperties":false)"
            R"(})");
}

void registerMobileScreenCaptureTools(
    MaiToolRegistry& tools, const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    addMaiMobileHostTool(
        tools, "mobile_start_screen_share",
        "Ask the user to start a system-approved whole-device screen-sharing session. "
        "In the Android system picker, choose Share entire screen. "
        "The user can then switch to another app and return before requesting a frame.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        dispatcher, false);
    addMaiMobileHostTool(
        tools, "mobile_stop_screen_share",
        "Stop the active whole-device screen-sharing session and discard its cached frame.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        dispatcher, false);
    tools.add(std::make_unique<MaiMobileScreenCaptureTool>(dispatcher));
}

// 移动端相册和播放器工具复用同一个宿主调度器；具体权限与媒体生命周期由手机宿主处理。
// Schema 限定操作和范围，模型不能凭工具参数请求任意原生调用。
void registerMobilePhotoTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
#if defined(MAICHAT_MOBILE_IOS)
    tools.add(std::make_unique<MaiMobileHostTool>(
        "maichat_camera_command",
        "Control the iPhone camera preview without waiting for a photo. open returns immediately; "
        "the user takes the photo in the system camera UI. status reports the current state "
        "and last workspace photo path. close dismisses the preview; call it only when the "
        "user asks to close or the task ends. Never take a photo without the user's action.",
        R"({"type":"object","properties":{"action":{"type":"string",)"
            R"("enum":["open","status","close"]}},"required":["action"],)"
            R"("additionalProperties":false})",
        dispatcher, false));
#else
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_take_photo",
        "Open the device camera preview and wait for the user to take a photo. Save a new JPEG "
        "in the current Agent workspace. Cancel creates no file and never sends a message.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        dispatcher, false));
#endif
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_play_local_music",
        "Play an MP3, M4A, or WAV with MaiChat's FFplay music player. Provide a relative "
        "workspace path, or omit it so the user chooses the system music library or a file. "
        "Protected Apple Music songs use Apple's system player in the same MaiChat UI. "
        "Never scan the device music library without user selection.",
        R"({"type":"object","properties":{"path":{"type":"string"}},)"
            R"("additionalProperties":false})",
        dispatcher, false));
    // 相册工作流：先授权并列出素材；预览用有界 JPEG，任何编辑/转码都先导出原文件。
    // 处理工具创建新文件；只有用户需要加入相册时再调用保存工具，原素材始终保留。
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_list_photos",
        "List photos, Live Photos, and videos visible in the system library after OS authorization. "
        "Each item includes mediaType; use offset and limit to page. A limited grant exposes "
        "only the user's selected media.",
        // offset/limit 分页；album_id 可将范围限制在一个相册。
        R"({"type":"object","properties":{"offset":{"type":"integer","minimum":0},"limit":{"type":"integer",)"
            R"("minimum":1,"maximum":100},"album_id":{"type":"string"}}})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_list_albums",
        "List system photo albums available under the current OS photo-library permission.",
        // 无参数，结果仍受系统相册授权范围限制。
        R"({"type":"object","properties":{}})", dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_read_photo",
        // 预览图最多 2048 像素，可能丢掉宽色域信息，不能用它充当编辑源图。
        "Copy one bounded JPEG preview by ID into the Agent working directory (up to 2048px). "
        "This loses original resolution and wide-gamut metadata; never use it as an editing "
        "source. Use mobile_export_photo_original before image processing. Use "
        "mobile_preview_image to show it to the user, or view_image with a vision-capable model. "
        "Only images authorized by the OS photo-library permission can be read.",
        // id 是相册项目 ID；返回的是有界预览副本，不能用作后续编辑输入。
        R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_export_photo_original",
        // 编辑或给视频模型提交首帧时导出原件；Live Photo 可明确选择静图或配对视频。
        "Copy the original photo or video bytes by ID from the authorized system library into "
        "the Agent working directory, preserving the source format and color metadata. Use this "
        "for any image edit or media conversion. The library item stays unchanged. For an iOS Live Photo, "
        "component=video exports its paired video; otherwise its still photo is exported.",
        // id 是相册项目 ID；Live Photo 的 component=video 取配对视频，省略取静图。
        R"({"type":"object","properties":{"id":{"type":"string"},"component":{"type":"string","enum":["photo","video"])"
            R"(}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_export_media_original",
        "Export the original photo or video file from the authorized system library into the "
        "Agent workspace, preserving its media format. Use this for videos before ffprobe, "
        "ffmpeg, or playback. A Live Photo exports its still image unless component=video.",
        // 与原图导出共用素材 ID/Live Photo component 约定；视频分析应选原始视频。
        R"({"type":"object","properties":{"id":{"type":"string"},"component":{"type":"string","enum":["photo","video"])"
            R"(}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_save_image",
        // 保存动作创建新相册项目，不能覆盖输入文件；时间按发起保存命令的时刻写入。
        "Save an image from the Agent working directory as a NEW photo in the system library. "
        "The source file and all existing library photos remain unchanged. Pass a relative path "
        "inside the Agent working directory.",
        // path 是工作区已存在的图片；保存时新建相册项，不覆盖原文件。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_save_video",
        // 视频保存同样只新增相册项目，使用生成工具返回的确切路径。
        "Save a video file from the Agent workspace as a NEW video in the system photo library. "
        "The source file remains unchanged. Accepts MP4, MOV, and M4V up to 2 GB. "
        "Pass the exact workspace path returned by the video tool.",
        // path 要用视频工具返回的准确工作区路径；只接受 MP4/MOV/M4V。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_transform_image",
        // 一次调用只做一种端侧处理；先预览结果，再按需保存或交给选定的视频模型。
        "Create a NEW image in the Agent working directory using an on-device edit; the source "
        "is unchanged. Apply one operation per call: crop (top-left x,y,width,height), rotate "
        "(degrees 90/180/270 clockwise), resize (width,height), flip_horizontal, flip_vertical, "
        "grayscale, sharpen (amount 0..2), adjust (brightness -1..1, contrast 0..2, "
        "saturation 0..2), or beautify "
        "(strength 0..1, mild whole-image smoothing and brightening, not face-aware). Call "
        "mobile_preview_image on the returned path, then mobile_save_image to add the result "
        "to the system gallery if requested.",
        // operation 每次只做一项裁剪、旋转、缩放、翻转、灰度、锐化、调色或轻度美化。
        // 其余坐标/强度字段只对所选操作有效，执行层逐项校验范围。
        R"({"type":"object","properties":{"path":{"type":"string"},"operation":{"type":"string","enum":["crop",)"
            R"("rotate","resize","flip_horizontal","flip_vertical","grayscale","sharpen","adjust","beautify"]},)"
            R"("x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},)"
            R"("degrees":{"type":"integer"},"brightness":{"type":"number"},"contrast":{"type":"number"},)"
            R"("saturation":{"type":"number"},"strength":{"type":"number"},"amount":{"type":"number"}},)"
            R"("required":["path","operation"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_beautify_image",
        "Make a NEW portrait-friendly image using mild whole-image smoothing, brightening, and "
        "slightly richer colors. This is not face-aware skin retouching. Strength is 0..1. "
        "The source remains unchanged; preview the returned path before saving it.",
        // path 为输入；strength 取 0–1。结果是新图，属于整图轻度处理而非脸部识别。
        R"({"type":"object","properties":{"path":{"type":"string"},"strength":{"type":"number","minimum":0,)"
            R"("maximum":1}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_detect_faces",
        "Find faces and facial landmarks in an image from the Agent working directory using "
        "on-device vision. Return top-left pixel bounds and landmark positions for local image "
        "editing; this does not identify who a person is or modify the source image.",
        // path 为现有工作区图；只返回面框与关键点坐标，不返回人物身份。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_segment_person",
        "Create a grayscale person-versus-background mask from an image in the Agent working "
        "directory using on-device vision. White means person and black means background. "
        "Return a NEW mask PNG path and dimensions; do not modify the source image.",
        // path 为输入图；返回独立灰度遮罩，白色为人物、黑色为背景。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_preview_image",
        "Show an image from the Agent working directory to the user in a full-screen mobile "
        "preview. This does not require a vision-capable model and does not save or modify it. "
        "Use after mobile_read_photo or mobile_transform_image so the user can inspect it.",
        // path 为待展示图片；只全屏预览，不保存、不编辑。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    registerPlatformPhotoAlbumTools(tools, dispatcher);
}
