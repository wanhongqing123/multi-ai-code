import type { RemoteImStatus } from './types.js'

const ACCOUNT_CHANGED_DETAIL = 'Remote IM account changed; waiting for reconnect'

export function createRemoteImAccountChangedStatuses(
  statuses: Iterable<RemoteImStatus>,
  now = Date.now()
): RemoteImStatus[] {
  const nextStatuses: RemoteImStatus[] = []
  for (const status of statuses) {
    if (!status.projectId) continue
    if (status.state === 'disabled') {
      nextStatuses.push({
        ...status,
        updatedAt: now
      })
      continue
    }
    nextStatuses.push({
      projectId: status.projectId,
      state: 'disconnected',
      detail: ACCOUNT_CHANGED_DETAIL,
      updatedAt: now
    })
  }
  return nextStatuses
}

export function getRemoteImSendConnectionError(status: RemoteImStatus | null | undefined): string | null {
  if (status?.state === 'connected') return null
  const detail = status?.detail?.trim()
  return detail ? `Remote IM is not connected: ${detail}` : 'Remote IM is not connected'
}
