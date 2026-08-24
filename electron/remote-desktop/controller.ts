// 被控端粘合层：把 IM 信令、状态机决策和 TRTC 引擎串起来。
//
// 分工严格：
//   session.ts  只做判定，不产生副作用
//   engine.ts   只做副作用，不做判定
//   本文件      把判定翻译成副作用，并维护当前会话
//
// 本轮只做「看画面」，不含输入注入。因此这里也没有任何接收远端输入的入口。
// 同样，Multi-AI Code 只作被控端：没有发起邀请的方法，这个能力压根不存在。

import type {
  RemoteDesktopCaptureGeometry,
  RemoteDesktopSignal
} from './signal.js'
import { encodeRemoteDesktopSignal } from './signal.js'
import type { RemoteDesktopCaptureSource, RemoteDesktopEngine } from './engine.js'
import {
  decideOnInvite,
  decideOnPeerGone,
  type RemoteDesktopHostMode,
  type RemoteDesktopHostState
} from './session.js'

export interface RemoteDesktopSettingsSnapshot {
  mode: RemoteDesktopHostMode
  allowedUserIds: string[]
  /**
   * 是否允许对端操作本机键鼠。独立于 mode：无人值守只授权了"看"，
   * 不该顺带把整台电脑交出去，所以默认关闭、要单独开一次。
   */
  allowRemoteControl?: boolean
}

export interface RemoteDesktopControllerDeps {
  engine: RemoteDesktopEngine
  getSettings(): RemoteDesktopSettingsSnapshot
  /**
   * 在真正做准入判断前重新取一次设置。白名单来自 IM 好友名单，而好友随时会变
   * （imcli add-contact、界面添加都会改）；渲染进程手里的那份是缓存，一旦某次
   * 刷新没送到，就会拿着过期名单把人挡在门外，且只能靠重启恢复。
   * 准入是安全判断，宁可多一次往返也要用当下的权威数据。
   * 未提供时退回 getSettings()，老调用方不受影响。
   */
  refreshSettings?(): Promise<RemoteDesktopSettingsSnapshot | null>
  /** 拿本机 TRTC 凭证。复用 IM 的 sdkAppId / userId / secretKey，无需二次签名。 */
  getCredentials(): Promise<{ sdkAppId: number; userId: string; userSig: string }>
  /** 把信令文本经 IM 发回给对端。 */
  sendSignal(toUserId: string, text: string): Promise<void>
  /** 状态变化通知 UI（共享指示条靠它）。 */
  onStateChanged?(state: RemoteDesktopControllerState): void
  logger?(message: string, detail?: Record<string, unknown>): void
}

export interface RemoteDesktopControllerState {
  hostState: RemoteDesktopHostState
  peerUserId: string | null
  sessionId: string | null
}

function normalizeList(values: string[]): string[] {
  return values.map((value) => value.trim()).filter(Boolean)
}

/**
 * 采集源转成信令里的几何。当前只支持整屏采集，所以采集区就是整个源。
 * contentMode 固定 fit：与 MaiChat 的编码/解码约定一致。
 */
export function captureGeometryFromSource(
  source: RemoteDesktopCaptureSource,
  revision: number
): RemoteDesktopCaptureGeometry {
  return {
    sourceWidth: source.width,
    sourceHeight: source.height,
    captureX: 0,
    captureY: 0,
    captureWidth: source.width,
    captureHeight: source.height,
    contentMode: 'fit',
    revision
  }
}

export class RemoteDesktopController {
  private hostState: RemoteDesktopHostState = 'idle'
  private peerUserId: string | null = null
  private sessionId: string | null = null
  private geometryRevision = 0

  constructor(private readonly deps: RemoteDesktopControllerDeps) {}

  getState(): RemoteDesktopControllerState {
    return { hostState: this.hostState, peerUserId: this.peerUserId, sessionId: this.sessionId }
  }

  private setState(next: RemoteDesktopHostState, peerUserId: string | null, sessionId: string | null): void {
    this.hostState = next
    this.peerUserId = peerUserId
    this.sessionId = sessionId
    this.deps.onStateChanged?.(this.getState())
  }

  private async reply(toUserId: string, signal: RemoteDesktopSignal): Promise<void> {
    await this.deps.sendSignal(toUserId, encodeRemoteDesktopSignal(signal))
  }

  /** 收到对端信令。返回是否被当作远程桌面信令消费掉。 */
  async handleSignal(fromUserId: string, signal: RemoteDesktopSignal): Promise<boolean> {
    switch (signal.type) {
      case 'invite':
        await this.handleInvite(fromUserId, signal)
        return true
      case 'stop':
        await this.handlePeerGone(fromUserId)
        return true
      // accept / reject / notice 是主控端才会收到的方向。作为被控端收到它们
      // 说明对端把角色搞反了，或者是旧会话的残留——记下来但不做任何动作。
      case 'accept':
      case 'reject':
      case 'notice':
        this.deps.logger?.('remote-desktop: ignored viewer-side signal', {
          type: signal.type,
          fromUserId
        })
        return true
      default:
        return false
    }
  }

  /** 取最新设置；拿不到（未提供或出错）时返回 null，由调用方退回缓存。 */
  private async refreshedSettings(): Promise<RemoteDesktopSettingsSnapshot | null> {
    if (!this.deps.refreshSettings) return null
    try {
      return await this.deps.refreshSettings()
    } catch (error) {
      // 刷新失败不能让准入直接挂掉：退回缓存继续判断，但要留痕。
      this.deps.logger?.('remote-desktop: settings refresh failed', {
        error: error instanceof Error ? error.message : String(error)
      })
      return null
    }
  }

  private async handleInvite(fromUserId: string, signal: RemoteDesktopSignal): Promise<void> {
    // 先刷新再判断：缓存的白名单过期时，表现是「明明是好友却连不上，重启才好」。
    const settings = (await this.refreshedSettings()) ?? this.deps.getSettings()
    const allowedUserIds = normalizeList(settings.allowedUserIds)
    const allowed = allowedUserIds.includes(fromUserId.trim())
    const decision = decideOnInvite({
      mode: settings.mode,
      currentState: this.hostState,
      senderAllowed: allowed,
      // 本轮不做访问密码：白名单即授权，与 MaiChat 无人值守未设密码时行为一致。
      passwordConfigured: false,
      authProofValid: false,
      consecutiveFailures: 0
    })

    this.deps.logger?.('remote-desktop: invite decision', {
      fromUserId,
      action: decision.action,
      mode: settings.mode,
      allowed,
      // 把判断依据也记下来：只记 allowed=false 的话，排查时分不清是
      // 「名单里确实没有这个人」还是「名单本身是空的/过期的」。
      allowedUserIds
    })

    if (decision.action === 'rejectInvite') {
      await this.reply(fromUserId, {
        type: 'reject',
        sessionId: signal.sessionId,
        reason: decision.reason
      })
      return
    }

    // 本轮不实现有人值守弹窗；配置成 attended 时按拒绝处理，而不是静默不响应——
    // 对端必须拿到明确答复，否则会一直卡在"连接中"。
    if (decision.action === 'showConsentDialog') {
      await this.reply(fromUserId, {
        type: 'reject',
        sessionId: signal.sessionId,
        reason: '对方需要人工确认，但当前版本尚未支持'
      })
      return
    }

    if (decision.action !== 'acceptAndShare') return

    const roomId = (signal.roomId ?? '').trim()
    if (!roomId) {
      await this.reply(fromUserId, {
        type: 'reject',
        sessionId: signal.sessionId,
        reason: '邀请缺少房间号'
      })
      return
    }

    try {
      const sources = await this.deps.engine.listScreenSources()
      const source = sources.find((item) => item.isMainScreen) ?? sources[0]
      if (!source) throw new Error('没有可用的屏幕采集源')

      const credentials = await this.deps.getCredentials()
      await this.deps.engine.startSharing(
        { ...credentials, roomId },
        source
      )

      this.geometryRevision += 1
      this.setState('sharing', fromUserId, signal.sessionId ?? null)
      // 控制开关是独立授权：没开就连门禁都不下推，引擎那侧收到输入包直接丢。
      this.applyInputGate(settings, fromUserId, signal.sessionId ?? null, source)
      // 几何必须在 accept 里带上：主控端要靠它把远端坐标换算回真实像素，
      // 缺了它对端只能猜，区域采集或补边时会整体偏移。
      await this.reply(fromUserId, {
        type: 'accept',
        sessionId: signal.sessionId,
        roomId,
        captureGeometry: captureGeometryFromSource(source, this.geometryRevision)
      })
    } catch (error) {
      // 进房失败必须如实回拒，否则对端会一直卡在"连接中"。
      // 同时落一条日志：对端只看得到一句话，本机要留下能查的现场。
      this.deps.engine.setInputGate(null)
      this.deps.logger?.('remote-desktop: start sharing failed', {
        fromUserId,
        sessionId: signal.sessionId ?? null,
        roomId,
        error: error instanceof Error ? error.message : String(error),
        stack: error instanceof Error ? error.stack?.split('\n').slice(0, 4).join('\n') : undefined
      })
      this.setState('idle', null, null)
      await this.reply(fromUserId, {
        type: 'reject',
        sessionId: signal.sessionId,
        reason: error instanceof Error ? error.message : '启动屏幕共享失败'
      })
    }
  }

  /**
   * 共享成功后决定是否放开输入。
   *
   * 门禁只在这里下推一次，之后 30Hz 的输入全在引擎那侧处理——逐事件跨进程
   * 往返的延迟和开销都不可接受。判定仍用 session.ts 那套条件，没有第二份实现。
   */
  private applyInputGate(
    settings: RemoteDesktopSettingsSnapshot,
    peerUserId: string,
    sessionId: string | null,
    source: RemoteDesktopCaptureSource
  ): void {
    // 没开控制、或这场会话没有 ID（无从绑定），一律不放行。
    if (!settings.allowRemoteControl || !sessionId) {
      this.deps.engine.setInputGate(null)
      this.deps.logger?.('remote-desktop: remote control disabled', {
        allowRemoteControl: settings.allowRemoteControl === true,
        hasSessionId: Boolean(sessionId)
      })
      return
    }
    this.deps.engine.setInputGate({
      sessionId,
      peerUserId,
      // 采集的是整屏，所以采集区就是这块屏本身。归一化坐标按它换算回像素。
      captureScreen: { left: 0, top: 0, width: source.width, height: source.height }
    })
    this.deps.logger?.('remote-desktop: remote control enabled', { peerUserId, sessionId })
  }

  private async handlePeerGone(fromUserId: string): Promise<void> {
    // 只认当前会话的对端：别人发来的 stop 不能掐掉正在进行的共享。
    if (this.peerUserId && fromUserId.trim() !== this.peerUserId) return
    const decision = decideOnPeerGone(this.hostState)
    this.deps.engine.setInputGate(null)
    if (decision.action === 'stopSharing') await this.deps.engine.stopSharing()
    this.setState('idle', null, null)
  }

  /** 本地主动停止（设置里关掉、或用户点了指示条上的停止）。 */
  async stopByLocalUser(): Promise<void> {
    this.deps.engine.setInputGate(null)
    if (this.hostState !== 'sharing') {
      this.setState('idle', null, null)
      return
    }
    const peerUserId = this.peerUserId
    const sessionId = this.sessionId
    await this.deps.engine.stopSharing()
    this.setState('idle', null, null)
    if (peerUserId) {
      await this.reply(peerUserId, { type: 'stop', sessionId: sessionId ?? undefined })
    }
  }
}
