import type { RemoteImActivitySignal } from '../../electron/remote-im/types.js'

// Ephemeral, account-scoped presentation state. Never routes into AICLI or history.
const entries = new Map<string, RemoteImActivitySignal>()
const timers = new Map<string, ReturnType<typeof setTimeout>>()
const closed = new Set<string>()
const listeners = new Set<() => void>()
export const activityKey = (owner: string, peer: string): string => `${owner}\0${peer}`
const identity = (key: string, id: string): string => `${key}\0${id}`
function rememberClosed(key: string, id: string): void {
  closed.add(identity(key, id))
  if (closed.size > 512) closed.delete(closed.values().next().value!)
}
function changed(): void { for (const listener of listeners) listener() }
export function subscribeActivity(listener: () => void): () => void {
  listeners.add(listener)
  return () => { listeners.delete(listener) }
}
export function getActivity(key: string): RemoteImActivitySignal | undefined {
  return entries.get(key)
}
export function clearActivity(key: string): void {
  const current = entries.get(key)
  if (!current) return
  rememberClosed(key, current.activityId)
  clearTimeout(timers.get(key))
  timers.delete(key)
  entries.delete(key)
  changed()
}
export function receiveActivity(key: string, signal: RemoteImActivitySignal): void {
  if (closed.has(identity(key, signal.activityId))) return
  const current = entries.get(key)
  if (!signal.active) {
    rememberClosed(key, signal.activityId)
    if (current?.activityId === signal.activityId) clearActivity(key)
    return
  }
  if (current?.activityId === signal.activityId && current.sequence >= signal.sequence) return
  if (current && current.activityId !== signal.activityId) rememberClosed(key, current.activityId)
  clearTimeout(timers.get(key))
  // Only a phase change invalidates UI; heartbeats just extend the lease.
  const visibleChanged = !current || current.activityId !== signal.activityId || current.kind !== signal.kind
  entries.set(key, visibleChanged ? signal : { ...current, sequence: signal.sequence })
  timers.set(key, setTimeout(() => {
    timers.delete(key)
    entries.delete(key)
    changed()
  }, signal.ttlMs))
  if (visibleChanged) changed()
}
export function clearAccountActivities(owner: string): void {
  for (const key of entries.keys()) if (key.startsWith(`${owner}\0`)) clearActivity(key)
}
