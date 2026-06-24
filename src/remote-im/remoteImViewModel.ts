import type { RemoteImMessage, RemoteImStatus } from '../../electron/preload.js'

export function getRemoteImStatusLabel(status: RemoteImStatus | null): string {
  if (!status) return '未连接'
  switch (status.state) {
    case 'connected':
      return '已连接'
    case 'connecting':
      return '连接中'
    case 'disabled':
      return '未开启'
    case 'error':
      return '异常'
    case 'disconnected':
    default:
      return '未连接'
  }
}

export function getRemoteImMessageAvatar(message: Pick<RemoteImMessage, 'role'>): string {
  if (message.role === 'remote-user') return '手'
  if (message.role === 'system') return '系'
  return 'AI'
}

export function getRemoteImMessageAuthor(message: RemoteImMessage): string {
  if (message.role === 'remote-user') return message.fromUserId || '手机'
  if (message.role === 'system') return 'Multi-AI Code'
  return 'AICLI 输出'
}

export function getRemoteImMessageStatusLabel(message: RemoteImMessage): string {
  switch (message.status) {
    case 'received':
      return '已接收'
    case 'sent-to-aicli':
      return '已发送'
    case 'streaming':
      return '回复中'
    case 'sent-to-im':
      return '已回发'
    case 'rejected':
      return '已拒绝'
    case 'failed':
      return '失败'
    default:
      return ''
  }
}

export function formatRemoteImTime(timestamp: number): string {
  const date = new Date(timestamp)
  return `${String(date.getHours()).padStart(2, '0')}:${String(date.getMinutes()).padStart(
    2,
    '0'
  )}:${String(date.getSeconds()).padStart(2, '0')}`
}

export function isRemoteImSendDisabled(input: {
  projectId: string | null
  sessionRunning: boolean
  text: string
  status: RemoteImStatus | null
}): boolean {
  return !input.projectId || !input.sessionRunning || input.text.trim().length === 0
}
