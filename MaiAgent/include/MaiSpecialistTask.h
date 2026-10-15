// 文件作用：记录专用模型异步任务与状态转换。
// 对外类型：MaiSpecialistTaskStatus、MaiSpecialistTask、MaiSpecialistTaskStore；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiTime.h"

// 修订链中的一次专用模型任务。身份、原始意图、供应商任务 ID 与 parentTaskId 创建后不变；
// 执行状态和有界结果快照可以更新。此处不得存凭据、上传字节或父会话完整历史。
// 状态枚举以整数写入 SQLite；新增取值只能追加，不得重排已有值。
enum class MaiSpecialistTaskStatus {
    Submitted,
    Running,
    NeedsInput,
    Succeeded,
    Failed,
    Canceled,
};

struct MaiSpecialistTask {
    std::string id;
    std::string ownerSessionId;
    std::string specialistName;
    std::string parentTaskId;
    std::string providerTaskId;
    std::string intent;
    std::string contextSummary;
    std::string inputReference;
    std::string outputPath;
    MaiMillis created = 0;
    MaiSpecialistTaskStatus status = MaiSpecialistTaskStatus::Submitted;
    std::string partialText;
    std::string finalText;
    std::string errorText;
    MaiMillis lastCheckedAt = 0;
    std::string notificationMessageId;
    MaiMillis notificationAttemptAt = 0;
    MaiMillis notifiedAt = 0;
};

// 模型媒体工具只能使用这个窄存储接口。插入是原子操作，拒绝无所属会话、跨会话父任务和重复 ID。
// 查询必须同时匹配任务 ID 与所属会话；流式文本留在任务行，最终回复只向父会话交付一次。
class MaiSpecialistTaskStore {
public:
    virtual ~MaiSpecialistTaskStore() = default;
    virtual MaiError insertSpecialistTask(const MaiSpecialistTask& task) = 0;
    virtual bool getSpecialistTask(const std::string& id, const std::string& ownerSessionId,
                                   MaiSpecialistTask& out) const = 0;

    // 仅删除当前会话已结束且已通知的任务记录，不删除生成文件和聊天消息。
    // 未通知的任务还可能由后台线程交付；活跃任务还可能在云端计费，均不得静默丢弃。
    virtual MaiError deleteCompletedSpecialistTask(const std::string& taskId,
                                                   const std::string& ownerSessionId) = 0;

    virtual MaiError appendSpecialistText(const std::string& taskId,
                                          const std::string& ownerSessionId,
                                          const std::string& chunk, MaiMillis checkedAt) = 0;
    virtual MaiError updateSpecialistProgress(const std::string& taskId,
                                              const std::string& ownerSessionId,
                                              MaiMillis checkedAt) = 0;
    // 旧版本可能把一次状态查询错误误写成 Failed。只恢复带有云端任务 ID、且错误
    // 明确以 "Status check failed:" 开头的记录；清除旧通知状态后继续只读轮询。
    // 不触发新的付费提交，也不恢复供应商明确报告失败的任务。
    virtual MaiError recoverStatusCheckFailures() = 0;
    // 纯文本专用任务成功时可以没有路径；媒体生成工具标记 Succeeded 前必须确认产物存在。
    virtual MaiError finishSpecialistTask(const std::string& taskId,
                                          const std::string& ownerSessionId,
                                          MaiSpecialistTaskStatus status,
                                          const std::string& finalText,
                                          const std::string& outputPath, MaiMillis finishedAt) = 0;
    virtual std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t limit) const = 0;
    virtual std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(
        std::size_t limit) const = 0;
    // 通知前先预留稳定的 assistant 消息 ID；重试仍返回同一个 ID。
    // 父会话真正完成答复后才能标记通知完成；两个操作都拒绝其他会话冒领。
    virtual MaiResult<std::string> reserveSpecialistNotification(
        const std::string& taskId, const std::string& ownerSessionId,
        const std::string& proposedMessageId, MaiMillis attemptedAt) = 0;
    virtual MaiError markSpecialistNotified(const std::string& taskId,
                                            const std::string& ownerSessionId,
                                            const std::string& messageId, MaiMillis notifiedAt) = 0;
    // 已经有同一云任务的成功媒体投递时，允许跳过自动回执。只更新已完成任务的
    // 投递标记，不创建虚假的聊天消息；普通用户主动要求再次发送不受此限制。
    virtual MaiError markSpecialistResultAlreadyDelivered(const std::string& taskId,
                                                          const std::string& ownerSessionId,
                                                          MaiMillis notifiedAt) = 0;
};
