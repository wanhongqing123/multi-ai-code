import { useEffect, useRef } from 'react'
import type { RemoteImConfig, RemoteImLoginState } from '../../electron/preload.js'
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
import { connectTencentImClient, type TencentImRuntime } from './tencentImClient.js'

export interface RemoteImClientHostProps {
  projectId: string | null
  config: RemoteImConfig
  loginRequested: boolean
  onContactsSynced?: (payload: {
    config: RemoteImConfig
    loginState: RemoteImLoginState
  }) => void
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

export async function syncRemoteImContactUserIds(input: {
  projectId: string
  userIds: string[]
  syncContacts: (
    projectId: string,
    userIds: string[]
  ) => Promise<
    | { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState }
    | { ok: false; error: string }
  >
  onContactsSynced?: (payload: {
    config: RemoteImConfig
    loginState: RemoteImLoginState
  }) => void
}): Promise<void> {
  const userIds = normalizeRuntimeFriendUserIds(input.userIds)
  if (userIds.length === 0) return
  const result = await input.syncContacts(input.projectId, userIds)
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
    userIds: string[]
  ) => Promise<
    | { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState }
    | { ok: false; error: string }
  >
  onContactsSynced?: (payload: {
    config: RemoteImConfig
    loginState: RemoteImLoginState
  }) => void
}): Promise<void> {
  if (!input.runtime.listFriendUserIds) return
  await syncRemoteImContactUserIds({
    projectId: input.projectId,
    userIds: await input.runtime.listFriendUserIds(),
    syncContacts: input.syncContacts,
    onContactsSynced: input.onContactsSynced
  })
}

export default function RemoteImClientHost(props: RemoteImClientHostProps): null {
  const runtimeSlotRef = useRef(createRemoteImRuntimeSlot<TencentImRuntime>())
  const lifecycleQueueRef = useRef(createRemoteImLifecycleQueue())
  const onContactsSyncedRef = useRef(props.onContactsSynced)
  const connectionKey = getRemoteImConnectionKey(props)

  useEffect(() => {
    onContactsSyncedRef.current = props.onContactsSynced
  }, [props.onContactsSynced])

  useEffect(() => {
    let cancelled = false
    let ownedRuntime: TencentImRuntime | null = null

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

      await window.api.remoteIm.updateSdkStatus({
        projectId,
        state: 'connecting',
        detail: null
      })

      try {
        const runtime = await connectTencentImClient({
          projectId,
          config: props.config,
          onIncomingText: (message) => {
            void window.api.remoteIm.deliverIncomingText(message)
          },
          onIncomingAudio: (message) => {
            void window.api.remoteIm.deliverIncomingAudio(message)
          },
          onIncomingImage: (message) => {
            void window.api.remoteIm.deliverIncomingImage(message)
          },
          onIncomingFile: (message) => {
            void window.api.remoteIm.deliverIncomingFile(message)
          },
          onFriendListUpdated: (userIds) => {
            void syncRemoteImContactUserIds({
              projectId,
              userIds,
              syncContacts: window.api.remoteIm.syncContacts,
              onContactsSynced: onContactsSyncedRef.current
            }).catch((err) => {
              void window.api.remoteIm.writeRuntimeLog({
                projectId,
                sdkAppId: props.config.sdkAppId,
                desktopUserId: props.config.desktopUserId,
                event: 'friend-list:update-sync-failed',
                detail: { error: err instanceof Error ? err.message : String(err) }
              })
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
        await syncRemoteImContactsFromRuntime({
          projectId,
          runtime,
          syncContacts: window.api.remoteIm.syncContacts,
          onContactsSynced: onContactsSyncedRef.current
        }).catch((err) => {
          void window.api.remoteIm.writeRuntimeLog({
            projectId,
            sdkAppId: props.config.sdkAppId,
            desktopUserId: props.config.desktopUserId,
            event: 'friend-list:sync-failed',
            detail: { error: err instanceof Error ? err.message : String(err) }
          })
        })
        await window.api.remoteIm.updateSdkStatus({
          projectId,
          state: 'connected',
          detail: null
        })
      } catch (err) {
        await window.api.remoteIm.updateSdkStatus({
          projectId,
          state: 'error',
          detail: err instanceof Error ? err.message : String(err)
        })
      }
    }

    const enqueueLifecycle = lifecycleQueueRef.current
    const cancelScheduledConnect = scheduleRemoteImConnect(() => {
      void enqueueLifecycle(connect).catch(() => undefined)
    })

    const offOutgoing = window.api.remoteIm.onOutgoingText((evt) => {
      if (evt.projectId !== props.projectId) return
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
        return window.api.remoteIm.markOutgoingMessageFailed(evt.projectId, messageId, error)
      }
      void (async () => {
        try {
          const runtime = await runtimeSlotRef.current.waitForCurrent(
            OUTGOING_RUNTIME_WAIT_TIMEOUT_MS
          )
          await deliverRemoteImOutgoingText({
            runtime,
            event: evt,
            markSent: (messageId) =>
              window.api.remoteIm.markOutgoingMessageSent(evt.projectId, messageId),
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
        return window.api.remoteIm.markOutgoingMessageFailed(evt.projectId, messageId, error)
      }
      void (async () => {
        try {
          const runtime = await runtimeSlotRef.current.waitForCurrent(
            OUTGOING_RUNTIME_WAIT_TIMEOUT_MS
          )
          await deliverRemoteImOutgoingImage({
            runtime,
            event: evt,
            resolveFile: (event) =>
              event.fileToken ? resolveRemoteImOutgoingImageFile(event.fileToken) : null,
            markSent: (messageId) =>
              window.api.remoteIm.markOutgoingMessageSent(evt.projectId, messageId),
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
        return window.api.remoteIm.markOutgoingMessageFailed(evt.projectId, messageId, error)
      }
      void (async () => {
        try {
          const runtime = await runtimeSlotRef.current.waitForCurrent(
            OUTGOING_RUNTIME_WAIT_TIMEOUT_MS
          )
          await deliverRemoteImOutgoingFile({
            runtime,
            event: evt,
            markSent: (messageId) =>
              window.api.remoteIm.markOutgoingMessageSent(evt.projectId, messageId),
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
      void enqueueLifecycle(disconnectOwned).catch(() => undefined)
    }
  }, [connectionKey])

  return null
}
