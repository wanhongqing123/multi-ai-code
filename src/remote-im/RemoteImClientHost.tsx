import { useEffect, useRef } from 'react'
import type {
  RemoteImConfig,
  RemoteImLoginState,
  RemoteImRuntimeIdentity
} from '../../electron/preload.js'
import {
  deliverRemoteImOutgoingFile,
  deliverRemoteImOutgoingImage,
  deliverRemoteImOutgoingText
} from './outgoingDelivery.js'
import {
  forgetRemoteImOutgoingImageFile,
  resolveRemoteImOutgoingImageFile
} from './outgoingImageRegistry.js'
import { createRemoteImRuntimeSlot } from './remoteImRuntimeSlot.js'
import {
  connectTencentImClient,
  generateTencentUserSig,
  type TencentImRuntime
} from './tencentImClient.js'
import { createRemoteDesktopHost, type RemoteDesktopHost } from '../remote-desktop/host.js'
import type { RemoteDesktopSettingsSnapshot } from '../../electron/remote-desktop/controller'
import { createTrtcRemoteDesktopEngine } from '../remote-desktop/trtcEngine.js'
import type { RemoteDesktopControllerState } from '../../electron/remote-desktop/controller.js'

export interface RemoteImClientHostProps {
  projectId: string | null
  config: RemoteImConfig
  loginRequested: boolean
  onContactsSynced?: (payload: {
    config: RemoteImConfig
    loginState: RemoteImLoginState
  }) => void
  /**
   * 被控端共享状态。本组件是 headless 的（返回 null），指示条由父组件渲染——
   * 共享中必须有常驻可见提示，否则无人值守下你回到电脑前无从知道屏幕被看过。
   */
  onRemoteDesktopStateChanged?: (state: RemoteDesktopControllerState) => void
  /** 交出"本地停止共享"的入口，供指示条上的停止按钮调用。 */
  onRemoteDesktopHostReady?: (stop: () => Promise<void>) => void
}

const OUTGOING_RUNTIME_WAIT_TIMEOUT_MS = 15_000

export function scheduleRemoteImConnect(startConnect: () => void): () => void {
  let started = false
  const timer = setTimeout(() => {
    started = true
    startConnect()
  }, 0)
  return () => {
    if (!started) clearTimeout(timer)
  }
}

export function createRemoteImLifecycleQueue() {
  let queue = Promise.resolve()
  return (task: () => Promise<void> | void): Promise<void> => {
    const run = queue.then(task, task)
    queue = run.catch(() => undefined)
    return run
  }
}

export function getRemoteImConnectionKey(props: RemoteImClientHostProps): string {
  return JSON.stringify({
    projectId: props.projectId,
    loginRequested: props.loginRequested,
    provider: props.config.provider,
    sdkAppId: props.config.sdkAppId,
    desktopUserId: props.config.desktopUserId.trim(),
    userSigMode: props.config.userSigMode,
    userSigEndpoint: props.config.userSigEndpoint.trim(),
    userSigSecretKey: props.config.userSigSecretKey.trim()
  })
}

export function isSameRemoteImRuntimeIdentity(
  expected: RemoteImRuntimeIdentity,
  actual: RemoteImRuntimeIdentity
): boolean {
  return (
    expected.connectionId === actual.connectionId &&
    expected.desktopUserId.trim() === actual.desktopUserId.trim() &&
    expected.sdkAppId === actual.sdkAppId
  )
}

export function getRemoteImConnectionBlockReason(config: RemoteImConfig): string | null {
  if (!config.sdkAppId) return '请先选择 IM 应用配置'
  if (!config.desktopUserId.trim()) return '请先填写登录账号'
  if (config.userSigMode === 'secret-key' && !config.userSigSecretKey.trim()) {
    return '请先选择或填写连接凭证'
  }
  if (config.userSigMode === 'endpoint' && !config.userSigEndpoint.trim()) {
    return '请先填写凭证接口'
  }
  return null
}

export function shouldConnectRemoteImClient(props: RemoteImClientHostProps): boolean {
  return Boolean(
    props.loginRequested &&
      props.projectId &&
      !getRemoteImConnectionBlockReason(props.config)
  )
}

function normalizeRuntimeFriendUserIds(userIds: string[]): string[] {
  return Array.from(new Set(userIds.map((userId) => userId.trim()).filter(Boolean)))
}

/**
 * Coalesce friend-list refresh signals without allowing snapshots to overtake
 * each other. A signal received while a refresh is in flight marks the result
 * dirty and forces one more authoritative snapshot after it completes.
 */
export function createRemoteImFriendSnapshotSynchronizer(
  refresh: () => Promise<void>
): () => Promise<void> {
  let running: Promise<void> | null = null
  let refreshAgain = false

  return async () => {
    if (running) {
      refreshAgain = true
      return running
    }

    const next = (async () => {
      let lastError: unknown = null
      do {
        refreshAgain = false
        try {
          await refresh()
          lastError = null
        } catch (error) {
          lastError = error
        }
      } while (refreshAgain)
      if (lastError) throw lastError
    })()
    running = next
    try {
      await next
    } finally {
      if (running === next) running = null
    }
  }
}

export async function syncRemoteImContactUserIds(input: {
  projectId: string
  userIds: string[]
  syncContacts: (
    projectId: string,
    userIds: string[],
    runtimeIdentity: RemoteImRuntimeIdentity
  ) => Promise<
    | { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState }
    | { ok: false; error: string }
  >
  onContactsSynced?: (payload: {
    config: RemoteImConfig
    loginState: RemoteImLoginState
  }) => void
  runtimeIdentity: RemoteImRuntimeIdentity
}): Promise<void> {
  const userIds = normalizeRuntimeFriendUserIds(input.userIds)
  const result = await input.syncContacts(
    input.projectId,
    userIds,
    input.runtimeIdentity
  )
  if (!result.ok) return
  input.onContactsSynced?.({
    config: result.value,
    loginState: result.loginState
  })
}

export async function syncRemoteImContactsFromRuntime(input: {
  projectId: string
  runtime: Pick<TencentImRuntime, 'listFriendUserIds'>
  syncContacts: (
    projectId: string,
    userIds: string[],
    runtimeIdentity: RemoteImRuntimeIdentity
  ) => Promise<
    | { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState }
    | { ok: false; error: string }
  >
  onContactsSynced?: (payload: {
    config: RemoteImConfig
    loginState: RemoteImLoginState
  }) => void
  runtimeIdentity: RemoteImRuntimeIdentity
  isCurrent?: () => boolean
}): Promise<void> {
  if (!input.runtime.listFriendUserIds) return
  const userIds = await input.runtime.listFriendUserIds()
  if (input.isCurrent && !input.isCurrent()) return
  await syncRemoteImContactUserIds({
    projectId: input.projectId,
    userIds,
    syncContacts: input.syncContacts,
    onContactsSynced: input.onContactsSynced,
    runtimeIdentity: input.runtimeIdentity
  })
}

export default function RemoteImClientHost(props: RemoteImClientHostProps): null {
  const runtimeSlotRef = useRef(createRemoteImRuntimeSlot<TencentImRuntime>())
  const lifecycleQueueRef = useRef(createRemoteImLifecycleQueue())
  const onContactsSyncedRef = useRef(props.onContactsSynced)
  const onRemoteDesktopStateChangedRef = useRef(props.onRemoteDesktopStateChanged)
  const remoteDesktopHostRef = useRef<RemoteDesktopHost | null>(null)
  // 改远程桌面开关不该重连 IM，所以它不在 connectionKey 里；代价是被控端不能
  // 闭包捕获建连那一刻的 props，否则在设置里关掉它、连接不变就一直是旧值。
  const remoteDesktopSettingsRef = useRef<RemoteDesktopSettingsSnapshot>({
    mode: props.config.remoteDesktopMode,
    allowedUserIds: props.config.allowedUserIds
  })
  const connectionKey = getRemoteImConnectionKey(props)

  useEffect(() => {
    onContactsSyncedRef.current = props.onContactsSynced
  }, [props.onContactsSynced])

  useEffect(() => {
    onRemoteDesktopStateChangedRef.current = props.onRemoteDesktopStateChanged
  }, [props.onRemoteDesktopStateChanged])

  useEffect(() => {
    remoteDesktopSettingsRef.current = {
      mode: props.config.remoteDesktopMode,
      allowedUserIds: props.config.allowedUserIds
    }
  }, [props.config.remoteDesktopMode, props.config.allowedUserIds])

  useEffect(() => {
    let cancelled = false
    let ownedRuntime: TencentImRuntime | null = null
    let runtimeRegistered = false
    const runtimeIdentity: RemoteImRuntimeIdentity = {
      connectionId: globalThis.crypto.randomUUID(),
      desktopUserId: props.config.desktopUserId.trim(),
      sdkAppId: props.config.sdkAppId
    }
    // 远程桌面被控端。整个功能都在渲染进程：IM 消息最先到这里，而 TRTC SDK
    // 也只能在这里加载，同侧就不需要主进程↔渲染进程的请求/响应桥。
    // 信令在这里被消费掉、不再往主进程转发，因此不可能被 router 当成普通消息
    // 路由给 AICLI。
    const remoteDesktopHost = createRemoteDesktopHost({
      engine: createTrtcRemoteDesktopEngine(),
      getSettings: () => remoteDesktopSettingsRef.current,
      getCredentials: async () => ({
        sdkAppId: props.config.sdkAppId ?? 0,
        userId: props.config.desktopUserId.trim(),
        // 复用 IM 的签名：TRTC 与 IM 同 sdkAppId、同 userId，一个 sig 两边都认。
        userSig: await generateTencentUserSig({
          sdkAppId: props.config.sdkAppId ?? 0,
          userId: props.config.desktopUserId.trim(),
          secretKey: props.config.userSigSecretKey
        })
      }),
      sendText: async (toUserId, text) => {
        const runtime = ownedRuntime
        if (!runtime) throw new Error('IM 连接尚未就绪')
        await runtime.sendText(toUserId, text)
      },
      onStateChanged: (state) => {
        if (!cancelled) onRemoteDesktopStateChangedRef.current?.(state)
      },
      logger: (message, detail) => {
        const projectId = props.projectId
        if (!projectId) return
        void window.api.remoteIm.writeRuntimeLog({
          projectId,
          sdkAppId: props.config.sdkAppId,
          desktopUserId: props.config.desktopUserId,
          event: message,
          detail: detail ?? {}
        })
      }
    })
    remoteDesktopHostRef.current = remoteDesktopHost
    props.onRemoteDesktopHostReady?.(() => remoteDesktopHost.stopByLocalUser())

    const syncFriendSnapshot = createRemoteImFriendSnapshotSynchronizer(async () => {
      const projectId = props.projectId
      const runtime = ownedRuntime
      if (cancelled || !projectId || !runtime) return
      await syncRemoteImContactsFromRuntime({
        projectId,
        runtime,
        syncContacts: window.api.remoteIm.syncContacts,
        onContactsSynced: onContactsSyncedRef.current,
        runtimeIdentity,
        isCurrent: () => !cancelled && ownedRuntime === runtime
      })
    })

    function reportFriendSyncFailure(projectId: string, error: unknown): void {
      void window.api.remoteIm.writeRuntimeLog({
        projectId,
        sdkAppId: props.config.sdkAppId,
        desktopUserId: props.config.desktopUserId,
        event: 'friend-list:sync-failed',
        detail: { error: error instanceof Error ? error.message : String(error) }
      })
    }

    async function disconnectCurrent(): Promise<void> {
      await runtimeSlotRef.current.disconnectCurrent().catch(() => undefined)
    }

    async function disconnectOwned(): Promise<void> {
      await runtimeSlotRef.current.disconnectOwned(ownedRuntime).catch(() => undefined)
      ownedRuntime = null
    }

    async function connect(): Promise<void> {
      await disconnectCurrent()
      const projectId = props.projectId
      if (!shouldConnectRemoteImClient(props)) {
        if (projectId) {
          const blockReason = getRemoteImConnectionBlockReason(props.config)
          await window.api.remoteIm.updateSdkStatus({
            projectId,
            state: 'disconnected',
            detail: !props.loginRequested ? '等待登录' : blockReason
          })
        }
        return
      }
      if (!projectId) return

      try {
        const registered = await window.api.remoteIm.registerRuntime(projectId, runtimeIdentity)
        if (!registered.ok) throw new Error(registered.error ?? '远程 IM 连接身份注册失败')
        runtimeRegistered = true
        if (cancelled) return
        await window.api.remoteIm.updateSdkRuntimeStatus({
          projectId,
          state: 'connecting',
          detail: null
        }, runtimeIdentity)
        const runtime = await connectTencentImClient({
          projectId,
          config: props.config,
          onIncomingText: (message) => {
            if (cancelled) return
            // 远程桌面信令在这里消费掉，绝不能继续往主进程转发——否则会被
            // router 当成普通消息喂给 AICLI。
            if (remoteDesktopHost.handleIncomingText(message)) return
            void window.api.remoteIm.deliverIncomingText(message, runtimeIdentity)
          },
          onIncomingAudio: (message) => {
            if (!cancelled) void window.api.remoteIm.deliverIncomingAudio(message, runtimeIdentity)
          },
          onIncomingImage: (message) => {
            if (!cancelled) void window.api.remoteIm.deliverIncomingImage(message, runtimeIdentity)
          },
          onIncomingFile: (message) => {
            if (!cancelled) void window.api.remoteIm.deliverIncomingFile(message, runtimeIdentity)
          },
          onFriendListUpdated: () => {
            if (cancelled) return
            const runtime = ownedRuntime
            // An update can theoretically arrive while connectTencentImClient
            // is still returning. The unconditional initial snapshot below
            // covers that window; every later update refreshes the authoritative
            // SDK snapshot, including a successful empty friend list.
            if (!runtime) return
            void syncFriendSnapshot().catch((error) => {
              reportFriendSyncFailure(projectId, error)
            })
          },
          onRuntimeLog: (entry) => {
            void window.api.remoteIm.writeRuntimeLog(entry)
          }
        })
        if (cancelled) {
          await runtime.disconnect().catch(() => undefined)
          return
        }
        ownedRuntime = runtime
        runtimeSlotRef.current.setCurrent(runtime)
        await syncFriendSnapshot().catch((error) => {
          reportFriendSyncFailure(projectId, error)
        })
        // 漫游补拉：把离线期间的历史消息补进本地库（按 remoteMessageId 去重，
        // 只入库展示、不路由 AICLI）。后台进行，失败不影响连接可用性。
        void runtime
          .listRoamedTextMessages?.()
          .then((messages) => {
            if (cancelled || !messages.length) return
            return window.api.remoteIm.backfillRoamedText({
              projectId,
              messages,
              runtimeIdentity
            })
          })
          .catch((err) => {
            void window.api.remoteIm.writeRuntimeLog({
              projectId,
              sdkAppId: props.config.sdkAppId,
              desktopUserId: props.config.desktopUserId,
              event: 'roam:backfill-failed',
              detail: { error: err instanceof Error ? err.message : String(err) }
            })
          })
        await window.api.remoteIm.updateSdkRuntimeStatus({
          projectId,
          state: 'connected',
          detail: null
        }, runtimeIdentity)
      } catch (err) {
        if (cancelled) return
        const status = {
          projectId,
          state: 'error' as const,
          detail: err instanceof Error ? err.message : String(err)
        }
        if (runtimeRegistered) {
          await window.api.remoteIm.updateSdkRuntimeStatus(status, runtimeIdentity)
        } else {
          await window.api.remoteIm.updateSdkStatus(status)
        }
      }
    }

    const enqueueLifecycle = lifecycleQueueRef.current
    const cancelScheduledConnect = scheduleRemoteImConnect(() => {
      void enqueueLifecycle(connect).catch(() => undefined)
    })

    const offOutgoing = window.api.remoteIm.onOutgoingText((evt) => {
      if (evt.projectId !== props.projectId) return
      if (!isSameRemoteImRuntimeIdentity(evt.runtimeIdentity, runtimeIdentity)) return
      const markFailed = (messageId: number, error: string) => {
        void window.api.remoteIm.writeRuntimeLog({
          projectId: evt.projectId,
          sdkAppId: props.config.sdkAppId,
          desktopUserId: props.config.desktopUserId,
          peerUserId: evt.toUserId,
          messageId,
          event: 'send:delivery-failed',
          detail: { error }
        })
        return window.api.remoteIm.markOutgoingMessageFailed(
          evt.projectId,
          messageId,
          error,
          runtimeIdentity
        )
      }
      void (async () => {
        try {
          const runtime = await runtimeSlotRef.current.waitForCurrent(
            OUTGOING_RUNTIME_WAIT_TIMEOUT_MS
          )
          if (cancelled || ownedRuntime !== runtime) {
            throw new Error('Remote IM runtime changed before text delivery')
          }
          await deliverRemoteImOutgoingText({
            runtime,
            event: evt,
            markSent: (messageId, remoteMessageId) =>
              window.api.remoteIm.markOutgoingMessageSent(
                evt.projectId,
                messageId,
                remoteMessageId,
                runtimeIdentity
              ),
            markFailed
          })
        } catch (err) {
          if (!evt.messageId) return
          await markFailed(
            evt.messageId,
            err instanceof Error ? err.message : String(err)
          )
          void window.api.remoteIm.writeRuntimeLog({
            projectId: evt.projectId,
            sdkAppId: props.config.sdkAppId,
            desktopUserId: props.config.desktopUserId,
            peerUserId: evt.toUserId,
            messageId: evt.messageId,
            event: 'send:runtime-wait-failed',
            detail: { error: err instanceof Error ? err.message : String(err) }
          })
        }
      })()
    })

    const offOutgoingImage = window.api.remoteIm.onOutgoingImage((evt) => {
      if (evt.projectId !== props.projectId) return
      if (!isSameRemoteImRuntimeIdentity(evt.runtimeIdentity, runtimeIdentity)) return
      const markFailed = (messageId: number, error: string) => {
        void window.api.remoteIm.writeRuntimeLog({
          projectId: evt.projectId,
          sdkAppId: props.config.sdkAppId,
          desktopUserId: props.config.desktopUserId,
          peerUserId: evt.toUserId,
          messageId,
          event: 'send:image:delivery-failed',
          detail: { error }
        })
        return window.api.remoteIm.markOutgoingMessageFailed(
          evt.projectId,
          messageId,
          error,
          runtimeIdentity
        )
      }
      void (async () => {
        try {
          const runtime = await runtimeSlotRef.current.waitForCurrent(
            OUTGOING_RUNTIME_WAIT_TIMEOUT_MS
          )
          if (cancelled || ownedRuntime !== runtime) {
            throw new Error('Remote IM runtime changed before image delivery')
          }
          await deliverRemoteImOutgoingImage({
            runtime,
            event: evt,
            resolveFile: (event) =>
              event.fileToken ? resolveRemoteImOutgoingImageFile(event.fileToken) : null,
            markSent: (messageId, remoteMessageId) =>
              window.api.remoteIm.markOutgoingMessageSent(
                evt.projectId,
                messageId,
                remoteMessageId,
                runtimeIdentity
              ),
            markFailed
          })
        } catch (err) {
          if (!evt.messageId) return
          await markFailed(
            evt.messageId,
            err instanceof Error ? err.message : String(err)
          )
          void window.api.remoteIm.writeRuntimeLog({
            projectId: evt.projectId,
            sdkAppId: props.config.sdkAppId,
            desktopUserId: props.config.desktopUserId,
            peerUserId: evt.toUserId,
            messageId: evt.messageId,
            event: 'send:image:runtime-wait-failed',
            detail: { error: err instanceof Error ? err.message : String(err) }
          })
        } finally {
          if (evt.fileToken) forgetRemoteImOutgoingImageFile(evt.fileToken)
        }
      })()
    })

    const offOutgoingFile = window.api.remoteIm.onOutgoingFile((evt) => {
      if (evt.projectId !== props.projectId) return
      if (!isSameRemoteImRuntimeIdentity(evt.runtimeIdentity, runtimeIdentity)) return
      const markFailed = (messageId: number, error: string) => {
        void window.api.remoteIm.writeRuntimeLog({
          projectId: evt.projectId,
          sdkAppId: props.config.sdkAppId,
          desktopUserId: props.config.desktopUserId,
          peerUserId: evt.toUserId,
          messageId,
          event: 'send:file:delivery-failed',
          detail: { error }
        })
        return window.api.remoteIm.markOutgoingMessageFailed(
          evt.projectId,
          messageId,
          error,
          runtimeIdentity
        )
      }
      void (async () => {
        try {
          const runtime = await runtimeSlotRef.current.waitForCurrent(
            OUTGOING_RUNTIME_WAIT_TIMEOUT_MS
          )
          if (cancelled || ownedRuntime !== runtime) {
            throw new Error('Remote IM runtime changed before file delivery')
          }
          await deliverRemoteImOutgoingFile({
            runtime,
            event: evt,
            markSent: (messageId, remoteMessageId) =>
              window.api.remoteIm.markOutgoingMessageSent(
                evt.projectId,
                messageId,
                remoteMessageId,
                runtimeIdentity
              ),
            markFailed
          })
        } catch (err) {
          if (!evt.messageId) return
          await markFailed(
            evt.messageId,
            err instanceof Error ? err.message : String(err)
          )
          void window.api.remoteIm.writeRuntimeLog({
            projectId: evt.projectId,
            sdkAppId: props.config.sdkAppId,
            desktopUserId: props.config.desktopUserId,
            peerUserId: evt.toUserId,
            messageId: evt.messageId,
            event: 'send:file:runtime-wait-failed',
            detail: { error: err instanceof Error ? err.message : String(err) }
          })
        }
      })()
    })

    return () => {
      cancelled = true
      cancelScheduledConnect()
      offOutgoing()
      offOutgoingImage()
      offOutgoingFile()
      // IM 断开时必须停掉共享：连接没了就再也发不出 stop 信令，
      // 留着推流等于在用户不知情的情况下继续共享屏幕。
      void remoteDesktopHost.stopByLocalUser().catch(() => undefined)
      remoteDesktopHostRef.current = null
      void enqueueLifecycle(disconnectOwned).catch(() => undefined)
    }
  }, [connectionKey])

  return null
}
