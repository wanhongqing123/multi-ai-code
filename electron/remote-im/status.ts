import type { RemoteImStatus } from './types.js'

const ACCOUNT_CHANGED_DETAIL = '远程 IM 账号已变更，正在重新连接'

function sanitizeRemoteImStatusDetail(detail: string): string {
  return detail
    .replace(/Tencent IM/g, 'IM')
    .replace(/SDKAppID/g, 'IM 应用配置')
    .replace(/UserSig endpoint/gi, '凭证接口')
    .replace(/UserSig|usersig/gi, '登录凭证')
    .replace(/SecretKey/g, '连接凭证')
    .replace(/\bIM login failed\b/g, 'IM 登录失败')
    .replace(/\bIM send failed\b/g, 'IM 发送失败')
    .replace(/\bIM runtime is not connected\b/g, 'IM 运行时未连接')
    .replace(/\bIM send timed out\b/g, 'IM 发送超时')
    .replace(/invalid 登录凭证/g, '登录凭证无效')
}

export function createRemoteImAccountChangedStatuses(
  statuses: Iterable<RemoteImStatus>,
  now = Date.now()
): RemoteImStatus[] {
  const nextStatuses: RemoteImStatus[] = []
  for (const status of statuses) {
    if (!status.projectId) continue
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
  return detail ? `远程 IM 未连接：${sanitizeRemoteImStatusDetail(detail)}` : '远程 IM 未连接'
}
