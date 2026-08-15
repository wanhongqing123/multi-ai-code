// 被控端状态机：纯逻辑，不碰 TRTC、不碰注入、不碰 UI。
//
// 对照 MaiChat/desktop/src/remote/RemoteDesktopSession.cpp 写成，判定顺序与拒绝原因
// 都与之一致——两端行为不一致会让"为什么它拒绝我"变得极难排查。
//
// 只实现被控端（Host）一半。Multi-AI Code 按产品定位不能作为主控端，所以这里
// 根本没有 Viewer 状态机、也没有任何发起邀请的入口：不是靠 UI 藏起来，而是
// 这个能力压根不存在。
//
// 所有安全判断都收敛在本文件：「什么情况下别人能看我的屏幕 / 操作我的电脑」
// 必须只有一个地方需要读。

/** 被控端模式，对应设置页三态。 */
export type RemoteDesktopHostMode =
  /** 不接受任何请求。 */
  | 'disabled'
  /** 有人值守：每次弹窗确认。 */
  | 'attended'
  /** 无人值守：白名单（+ 可选密码）通过则自动接受。 */
  | 'unattended'

export type RemoteDesktopHostState = 'idle' | 'awaitingConsent' | 'sharing'

/** 状态机只描述"该做什么"，副作用由 controller 执行。 */
export type RemoteDesktopHostAction =
  | 'none'
  | 'showConsentDialog'
  | 'acceptAndShare'
  | 'rejectInvite'
  | 'stopSharing'

export interface RemoteDesktopHostDecision {
  action: RemoteDesktopHostAction
  nextState: RemoteDesktopHostState
  /** rejectInvite 时回给对端的原因。 */
  reason?: string
  /** 连续密码失败触发的模式降级。 */
  downgradeToAttended?: boolean
}

export interface RemoteDesktopInviteInput {
  mode: RemoteDesktopHostMode
  currentState: RemoteDesktopHostState
  /** 是否在白名单内。 */
  senderAllowed: boolean
  /** 是否设置了访问密码。密码是可选加固：白名单本身即授权。 */
  passwordConfigured: boolean
  /** 设了密码时，校验是否通过。校验由调用方算好传入，状态机不依赖加密实现。 */
  authProofValid: boolean
  consecutiveFailures: number
}

/** 拒绝原因与 MaiChat 逐字一致：主控端会原样展示给用户。 */
export const REMOTE_DESKTOP_REJECT_REASONS = {
  notEnabled: '对方未开启远程桌面',
  notAllowed: '你不在对方的允许列表中',
  busy: '对方正在共享中',
  badPassword: '访问密码错误',
  declined: '对方拒绝了请求',
  timeout: '对方未在规定时间内响应'
} as const

/** 与 MaiChat 的 kMaxConsecutiveFailures 一致：连续失败到此即降级，防在线爆破。 */
export const REMOTE_DESKTOP_MAX_CONSECUTIVE_FAILURES = 5

/** 有人值守弹窗的自动拒绝倒计时（毫秒）。无人应答时保持收紧。 */
export const REMOTE_DESKTOP_CONSENT_TIMEOUT_MS = 60000

export function shouldDowngradeToAttended(consecutiveFailures: number): boolean {
  return consecutiveFailures >= REMOTE_DESKTOP_MAX_CONSECUTIVE_FAILURES
}

export function decideOnInvite(input: RemoteDesktopInviteInput): RemoteDesktopHostDecision {
  // 已在共享中：不抢占已有会话。
  if (input.currentState !== 'idle') {
    return {
      action: 'rejectInvite',
      nextState: input.currentState,
      reason: REMOTE_DESKTOP_REJECT_REASONS.busy
    }
  }

  // 白名单先于模式判断：不在允许列表里的账号，连"对方开没开远程"都不该知道。
  // 顺序反过来会泄漏这台机器的配置状态。
  if (!input.senderAllowed) {
    return {
      action: 'rejectInvite',
      nextState: 'idle',
      reason: REMOTE_DESKTOP_REJECT_REASONS.notAllowed
    }
  }

  if (input.mode === 'disabled') {
    return {
      action: 'rejectInvite',
      nextState: 'idle',
      reason: REMOTE_DESKTOP_REJECT_REASONS.notEnabled
    }
  }

  if (input.mode === 'attended') {
    return { action: 'showConsentDialog', nextState: 'awaitingConsent' }
  }

  // 无人值守 + 未设密码：白名单即授权。主场景是"自己在外面连自己的电脑"，
  // 每次输密码的摩擦不值得——安全性由白名单与 IM 账号本身承担。
  if (!input.passwordConfigured) {
    return { action: 'acceptAndShare', nextState: 'sharing' }
  }

  if (!input.authProofValid) {
    return {
      action: 'rejectInvite',
      nextState: 'idle',
      reason: REMOTE_DESKTOP_REJECT_REASONS.badPassword,
      downgradeToAttended: shouldDowngradeToAttended(input.consecutiveFailures + 1)
    }
  }

  return { action: 'acceptAndShare', nextState: 'sharing' }
}

export function decideOnConsentResult(
  currentState: RemoteDesktopHostState,
  accepted: boolean
): RemoteDesktopHostDecision {
  // 只有正在等待确认时这个事件才有意义；乱序到达时保持原状态不动。
  if (currentState !== 'awaitingConsent') {
    return { action: 'none', nextState: currentState }
  }
  if (!accepted) {
    return {
      action: 'rejectInvite',
      nextState: 'idle',
      reason: REMOTE_DESKTOP_REJECT_REASONS.declined
    }
  }
  return { action: 'acceptAndShare', nextState: 'sharing' }
}

/** 收到对端 stop 或对端掉线。 */
export function decideOnPeerGone(
  currentState: RemoteDesktopHostState
): RemoteDesktopHostDecision {
  return {
    action: currentState === 'sharing' ? 'stopSharing' : 'none',
    nextState: 'idle'
  }
}

/**
 * 这一包远程输入该不该执行。
 *
 * 四个条件对外表现都是"远端鼠标不动"，排查时必须能区分是哪一个，
 * 所以返回具体判据而不是 boolean。
 */
export type RemoteDesktopInputVerdict =
  | 'accepted'
  | 'not-sharing'
  | 'control-disabled'
  | 'session-mismatch'
  | 'peer-mismatch'

export interface RemoteDesktopInputGateInput {
  currentState: RemoteDesktopHostState
  /** 独立于观看权限的开关，默认关：能看不等于能操作。 */
  allowRemoteControl: boolean
  currentSessionId: string
  currentPeerUserId: string
  packetSessionId: string
  fromUserId: string
}

export function remoteInputVerdict(
  input: RemoteDesktopInputGateInput
): RemoteDesktopInputVerdict {
  // 没在共享就不该有输入进来。
  if (input.currentState !== 'sharing') return 'not-sharing'
  // 控制权限是独立开关：无人值守只授权了"看"，不该顺带把整台电脑交出去。
  if (!input.allowRemoteControl) return 'control-disabled'
  // 会话 ID 必须对得上，挡掉上一场会话的残留包。
  if (!input.currentSessionId || input.packetSessionId !== input.currentSessionId) {
    return 'session-mismatch'
  }
  // 只认当前会话的对端：房间里若混进第三方，它的输入一律不执行。
  if (!input.currentPeerUserId || input.fromUserId !== input.currentPeerUserId) {
    return 'peer-mismatch'
  }
  return 'accepted'
}

/** shouldAcceptRemoteInput 是 remoteInputVerdict 的布尔化包装，两者不会各判一套。 */
export function shouldAcceptRemoteInput(input: RemoteDesktopInputGateInput): boolean {
  return remoteInputVerdict(input) === 'accepted'
}
