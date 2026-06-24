import { useEffect, useRef } from 'react'
import type { RemoteImConfig } from '../../electron/preload.js'
import { connectTencentImClient, type TencentImRuntime } from './tencentImClient.js'

export interface RemoteImClientHostProps {
  projectId: string | null
  config: RemoteImConfig
}

export default function RemoteImClientHost(props: RemoteImClientHostProps): null {
  const runtimeRef = useRef<TencentImRuntime | null>(null)

  useEffect(() => {
    let cancelled = false

    async function disconnect(): Promise<void> {
      const runtime = runtimeRef.current
      runtimeRef.current = null
      if (runtime) await runtime.disconnect().catch(() => undefined)
    }

    async function connect(): Promise<void> {
      await disconnect()
      if (!props.projectId || !props.config.enabled) {
        if (props.projectId) {
          await window.api.remoteIm.updateSdkStatus({
            projectId: props.projectId,
            state: 'disabled',
            detail: null
          })
        }
        return
      }

      await window.api.remoteIm.updateSdkStatus({
        projectId: props.projectId,
        state: 'connecting',
        detail: null
      })

      try {
        const runtime = await connectTencentImClient({
          projectId: props.projectId,
          config: props.config,
          onIncomingText: (message) => {
            void window.api.remoteIm.deliverIncomingText(message)
          }
        })
        if (cancelled) {
          await runtime.disconnect().catch(() => undefined)
          return
        }
        runtimeRef.current = runtime
        await window.api.remoteIm.updateSdkStatus({
          projectId: props.projectId,
          state: 'connected',
          detail: null
        })
      } catch (err) {
        await window.api.remoteIm.updateSdkStatus({
          projectId: props.projectId,
          state: 'error',
          detail: err instanceof Error ? err.message : String(err)
        })
      }
    }

    void connect()

    const offOutgoing = window.api.remoteIm.onOutgoingText((evt) => {
      if (evt.projectId !== props.projectId) return
      void runtimeRef.current?.sendText(evt.toUserId, evt.text)
    })

    return () => {
      cancelled = true
      offOutgoing()
      void disconnect()
    }
  }, [props.projectId, props.config])

  return null
}
