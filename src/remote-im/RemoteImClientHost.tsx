import { useEffect, useRef } from 'react'
import type { RemoteImConfig } from '../../electron/preload.js'
import { deliverRemoteImOutgoingText } from './outgoingDelivery.js'
import { createRemoteImRuntimeSlot } from './remoteImRuntimeSlot.js'
import { connectTencentImClient, type TencentImRuntime } from './tencentImClient.js'

export interface RemoteImClientHostProps {
  projectId: string | null
  config: RemoteImConfig
  loginRequested: boolean
}

const OUTGOING_RUNTIME_WAIT_TIMEOUT_MS = 15_000

export function getRemoteImConnectionKey(props: RemoteImClientHostProps): string {
  return JSON.stringify({
    projectId: props.projectId,
    loginRequested: props.loginRequested,
    enabled: props.config.enabled,
    provider: props.config.provider,
    sdkAppId: props.config.sdkAppId,
    desktopUserId: props.config.desktopUserId.trim(),
    userSigMode: props.config.userSigMode,
    userSigEndpoint: props.config.userSigEndpoint.trim(),
    userSigSecretKey: props.config.userSigSecretKey.trim()
  })
}

export function getRemoteImConnectionBlockReason(config: RemoteImConfig): string | null {
  if (!config.sdkAppId) return '请先选择 SDKAppID'
  if (!config.desktopUserId.trim()) return '请先填写 UserID'
  if (config.userSigMode === 'secret-key' && !config.userSigSecretKey.trim()) {
    return '请先选择凭证或填写 SecretKey'
  }
  if (config.userSigMode === 'endpoint' && !config.userSigEndpoint.trim()) {
    return '请先填写 UserSig endpoint'
  }
  return null
}

export function shouldConnectRemoteImClient(props: RemoteImClientHostProps): boolean {
  return Boolean(
    props.loginRequested &&
      props.projectId &&
      props.config.enabled &&
      !getRemoteImConnectionBlockReason(props.config)
  )
}

export default function RemoteImClientHost(props: RemoteImClientHostProps): null {
  const runtimeSlotRef = useRef(createRemoteImRuntimeSlot<TencentImRuntime>())
  const connectionKey = getRemoteImConnectionKey(props)

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
            state: props.config.enabled ? 'disconnected' : 'disabled',
            detail:
              props.config.enabled && !props.loginRequested
                ? '等待手动登录'
                : props.config.enabled && props.loginRequested
                  ? blockReason
                  : null
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

    void connect()

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

    return () => {
      cancelled = true
      offOutgoing()
      void disconnectOwned()
    }
  }, [connectionKey])

  return null
}
