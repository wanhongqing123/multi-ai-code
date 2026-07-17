import type { RemoteImMessage } from '../../electron/preload.js'

// 把「加载更早」翻页取回的历史消息并入当前列表：按 id 去重（已在列表中的
// 分页结果直接丢弃），并按 (createdAt, id) 升序重排，保证时间线稳定。
export function mergeRemoteImMessages(
  existing: RemoteImMessage[],
  fetched: RemoteImMessage[]
): RemoteImMessage[] {
  if (fetched.length === 0) return existing
  const seen = new Set(existing.map((message) => message.id))
  const fresh = fetched.filter((message) => !seen.has(message.id))
  if (fresh.length === 0) return existing
  return [...existing, ...fresh].sort(
    (a, b) => a.createdAt - b.createdAt || a.id - b.id
  )
}
