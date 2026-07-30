#pragma once

#include <QString>

#include "remote/RemoteDesktopSignal.h"

// 远程桌面的双端状态机：纯逻辑，不碰 Qt UI / 网络 / TRTC。
//
// 所有分支（模式判定、密码校验结果、超时、乱序信令）都在这里收敛，
// UI 只负责渲染状态和投递事件，controller 只负责把决策翻译成副作用。
// 这样"什么情况下会自动放行"这类安全关键逻辑可以被完整单测覆盖。
namespace RemoteDesktop {

// 被控端模式，对应设置页三态。
enum class HostMode {
    Disabled,    // 不接受任何请求
    Attended,    // 有人值守：每次弹窗确认（默认）
    Unattended   // 无人值守：白名单 + 密码校验通过则自动接受
};

// 控制端（发起方）状态。
enum class ViewerState {
    Idle,
    Inviting,    // 已发出 invite，等对方响应
    Connecting,  // 对方已同意，正在进房/等首帧
    Viewing,     // 画面已到达
    Failed       // 被拒 / 超时 / 出错，附 failureReason
};

// 被控端状态。
enum class HostState {
    Idle,
    AwaitingConsent,  // 有人值守：弹窗中，等用户点
    Sharing           // 推流中
};

// controller 需要执行的副作用。状态机只描述"该做什么"，不自己做。
enum class HostAction {
    None,
    ShowConsentDialog,   // 弹窗（有人值守）
    AcceptAndShare,      // 直接进房推流（无人值守通过，或用户点了同意）
    RejectInvite,        // 回 reject，reason 见 outcome
    StopSharing          // 停止推流并回 stop
};

struct HostDecision {
    HostAction action = HostAction::None;
    HostState nextState = HostState::Idle;
    QString reason;              // reject 时回给对端的原因
    bool downgradeToAttended = false;  // 连续失败触发的模式降级
};

// 被控端收到 invite 时的输入。密码校验结果由调用方（controller）算好传进来，
// 状态机本身不依赖加密实现，便于独立测试。
struct HostInviteInput {
    HostMode mode = HostMode::Attended;
    HostState currentState = HostState::Idle;
    bool senderAllowed = false;      // 是否在白名单内
    // 是否设置了访问密码。密码是**可选加固**：白名单本身即授权，
    // 用户想要额外一层时才设。没设密码不代表不安全，只是少一道锁。
    bool passwordConfigured = false;
    bool authProofValid = false;     // 设了密码时，校验是否通过
    int consecutiveFailures = 0;     // 此前连续失败次数
};

// 拒绝原因常量：UI 与测试共用，避免散落的字面量。
QString reasonNotEnabled();
QString reasonNotAllowed();
QString reasonBusy();
QString reasonBadPassword();
QString reasonDeclined();
QString reasonTimeout();

// 被控端：收到 invite 后该怎么办。
HostDecision decideOnInvite(const HostInviteInput& input);

// 被控端：这一包远程输入该不该执行。
//
// 放在这里而不是注入器里，是为了守住"所有安全判断只在本文件"的约定——
// "什么情况下别人能操作我的电脑"必须只有一个地方需要读。
//
// 四个条件全部满足才放行：
//   1. 正在共享（没在共享就不该有输入）
//   2. 设置里开了「允许远程控制」（默认关，独立于观看权限）
//   3. 会话 ID 对得上（拒收上一场会话的残留包）
//   4. 发来的人就是当前会话的对端
//
// 拒绝原因。四个条件对外表现都是"远端鼠标不动"，排查时必须能区分：
// 是没在共享、开关没开、还是包对不上号。
enum class InputVerdict {
    Accepted,
    NotSharing,        // 1
    ControlDisabled,   // 2
    SessionMismatch,   // 3
    PeerMismatch       // 4
};

// 供日志/诊断使用；shouldAcceptRemoteInput 就是它的布尔化包装，
// 两者不会各判一套。
InputVerdict remoteInputVerdict(HostState currentState, bool allowRemoteControl,
                                const QString& currentSessionId, const QString& currentPeerUserId,
                                const QString& packetSessionId, const QString& fromUserId);

// 以下三个都是日志用的短名，不面向用户，一律英文——日志文件要能在任何
// 编码环境下被原样读出来、能直接贴进 issue 搜索。
const char* inputVerdictName(InputVerdict verdict);
const char* hostStateName(HostState state);
const char* viewerStateName(ViewerState state);

bool shouldAcceptRemoteInput(HostState currentState, bool allowRemoteControl,
                             const QString& currentSessionId, const QString& currentPeerUserId,
                             const QString& packetSessionId, const QString& fromUserId);

// 被控端：有人值守弹窗的用户选择 / 超时。
HostDecision decideOnConsentResult(HostState currentState, bool accepted);

// 被控端：收到对端 stop 或对端掉线。
HostDecision decideOnPeerGone(HostState currentState);

// 控制端：状态推进。返回新状态；failureReason 仅在 Failed 时有意义。
struct ViewerTransition {
    ViewerState nextState = ViewerState::Idle;
    QString failureReason;
};

// 当前状态是否允许发起新邀请。把"能不能发"做成显式谓词，而不是让调用方
// 去比对 viewerOnInviteSent 的返回值——后者在已是 Inviting 时会原样返回，
// 极易被误读成"可以发"。
bool canStartInvite(ViewerState current);

ViewerTransition viewerOnInviteSent(ViewerState current);
ViewerTransition viewerOnSignal(ViewerState current, const RemoteDesktopSignals::Signal& signal);
ViewerTransition viewerOnFirstFrame(ViewerState current);
ViewerTransition viewerOnTimeout(ViewerState current);
ViewerTransition viewerOnLocalClose(ViewerState current);

// 邀请等待响应的超时（毫秒）。
constexpr int kInviteTimeoutMs = 30000;
// 有人值守弹窗的自动拒绝倒计时（毫秒）。
constexpr int kConsentTimeoutMs = 60000;

}  // namespace RemoteDesktop
