// 渲染进程侧的被控端入口。
//
// 为什么整个功能都放渲染进程：IM 消息最先到达这里（RemoteImClientHost 的
// onIncomingText），而 TRTC SDK 也只能在这里加载。两者同侧就不需要在主进程和
// 渲染进程之间架请求/响应桥——少一层就少一处会出错的地方。
//
// 附带的好处是：远程桌面信令在这里被消费掉、不再往主进程转发，所以它们不可能
// 被 router 当成普通消息路由给 AICLI。

import {
  RemoteDesktopController,
  type RemoteDesktopSettingsSnapshot
} from '../../electron/remote-desktop/controller.js'
import type { RemoteDesktopEngine } from '../../electron/remote-desktop/engine.js'
import {
  decodeRemoteDesktopSignal,
  isRemoteDesktopSignalText
} from '../../electron/remote-desktop/signal.js'
import type { RemoteDesktopControllerState } from '../../electron/remote-desktop/controller.js'

export interface RemoteDesktopHostIncomingText {
  projectId: string
  fromUserId: string
  text: string
}

export interface RemoteDesktopHostDeps {
  engine: RemoteDesktopEngine
  getSettings(): RemoteDesktopSettingsSnapshot
  /** 准入判断前重新取一次权威设置，避免缓存过期把好友挡在门外。 */
  refreshSettings?(): Promise<RemoteDesktopSettingsSnapshot | null>
  getCredentials(): Promise<{ sdkAppId: number; userId: string; userSig: string }>
  sendText(toUserId: string, text: string): Promise<void>
  onStateChanged?(state: RemoteDesktopControllerState): void
  logger?(message: string, detail?: Record<string, unknown>): void
}

export interface RemoteDesktopHost {
  /**
   * 返回 true 表示这条消息是远程桌面信令、已被消费，调用方**不应**再转发给主进程。
   * 返回 false 表示是普通消息，按原流程继续。
   */
  handleIncomingText(message: RemoteDesktopHostIncomingText): boolean
  getState(): RemoteDesktopControllerState
  stopByLocalUser(): Promise<void>
}

export function createRemoteDesktopHost(deps: RemoteDesktopHostDeps): RemoteDesktopHost {
  const controller = new RemoteDesktopController({
    engine: deps.engine,
    getSettings: deps.getSettings,
    refreshSettings: deps.refreshSettings,
    getCredentials: deps.getCredentials,
    sendSignal: deps.sendText,
    onStateChanged: deps.onStateChanged,
    logger: deps.logger
  })

  return {
    handleIncomingText(message) {
      // 先用前缀快速判断，避免对每条普通聊天都做 JSON 解析。
      if (!isRemoteDesktopSignalText(message.text)) return false
      const signal = decodeRemoteDesktopSignal(message.text)
      // 前缀对但内容不合法：当普通消息放行，与 MaiChat 的判定一致。
      if (!signal) return false
      // 信令处理是异步的，但调用方需要同步知道"要不要转发"，所以这里不 await。
      // 失败只记日志：这条链路的任何异常都不该影响普通消息的投递。
      void controller.handleSignal(message.fromUserId, signal).catch((error: unknown) => {
        deps.logger?.('remote-desktop: signal handling failed', {
          error: error instanceof Error ? error.message : String(error)
        })
      })
      return true
    },
    getState() {
      return controller.getState()
    },
    stopByLocalUser() {
      return controller.stopByLocalUser()
    }
  }
}
