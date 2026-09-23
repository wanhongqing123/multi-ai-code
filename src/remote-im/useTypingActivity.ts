import { useEffect, useRef } from 'react'
import type { RemoteImActivitySignal } from '../../electron/remote-im/types.js'

export function useTypingActivity(projectId: string | null, peer: string | null, enabled: boolean, owner: string) {
  const state = useRef<{
    id: string
    sequence: number
    lastSent: number
    startedAtMs: number
  } | null>(null)
  const idle = useRef<ReturnType<typeof setTimeout>>()
  const stop = () => {
    clearTimeout(idle.current)
    const current = state.current
    state.current = null
    if (current && projectId && peer) {
      void window.api.remoteIm.sendTypingActivity?.(projectId, peer, owner, {
        activityId: current.id, sequence: ++current.sequence, kind: 'human-typing',
        active: false, startedAtMs: current.startedAtMs, ttlMs: 12000
      }).catch(() => undefined)
    }
  }
  useEffect(() => stop, [projectId, peer, enabled, owner])
  return {
    stop,
    changed(text: string) {
      if (!enabled || !text || !projectId || !peer) { stop(); return }
      const current = state.current ?? {
        id: `typing:${crypto.randomUUID()}`, sequence: 0, lastSent: 0,
        startedAtMs: Date.now()
      }
      state.current = current
      clearTimeout(idle.current)
      idle.current = setTimeout(stop, 4000)
      if (Date.now() - current.lastSent < 3000) return
      current.lastSent = Date.now()
      const signal: RemoteImActivitySignal = {
        activityId: current.id, sequence: ++current.sequence, kind: 'human-typing',
        active: true, startedAtMs: current.startedAtMs, ttlMs: 12000
      }
      void window.api.remoteIm.sendTypingActivity?.(projectId, peer, owner, signal).catch(() => undefined)
    }
  }
}
