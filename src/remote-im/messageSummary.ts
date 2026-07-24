import type { RemoteImMessage } from '../../electron/preload.js'

// 把项目的全部 IM 消息记录汇总成一份 Markdown 文档：
// 顶部统计（总条数/会话数/时间范围），按会话分组（最近活跃在前），组内按时间升序。
// 消息正文本身多为 Markdown（AICLI 输出），原样嵌入、由渲染层统一渲染。

export interface RemoteImMessageSummaryOptions {
  ownerUserId?: string | null
}

function pad2(value: number): string {
  return String(value).padStart(2, '0')
}

export function formatSummaryTime(timestamp: number): string {
  const date = new Date(timestamp)
  return `${formatSummaryDay(timestamp)} ${pad2(date.getHours())}:${pad2(date.getMinutes())}`
}

export function formatSummaryDay(timestamp: number): string {
  const date = new Date(timestamp)
  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`
}

export function formatSummaryClock(timestamp: number): string {
  const date = new Date(timestamp)
  return `${pad2(date.getHours())}:${pad2(date.getMinutes())}`
}

// 会话对端：入站看发送者、出站看接收者；internal/system 归到有名字的一方，否则「系统」。
function peerOf(message: RemoteImMessage): string {
  if (message.direction === 'incoming') return message.fromUserId?.trim() || '未知会话'
  if (message.direction === 'outgoing') return message.toUserId?.trim() || '未知会话'
  return message.fromUserId?.trim() || message.toUserId?.trim() || '系统'
}

function senderLabel(message: RemoteImMessage, ownerUserId?: string | null): string {
  if (message.direction === 'incoming') return message.fromUserId?.trim() || '对方'
  if (message.direction === 'outgoing') return ownerUserId?.trim() || '我'
  return message.role === 'aicli' ? 'AICLI' : '系统'
}

function attachmentLine(message: RemoteImMessage): string | null {
  if (message.kind === 'image') {
    const fileName = message.attachment?.fileName ?? null
    return `📷 图片${fileName ? `：\`${fileName}\`` : ''}`
  }
  if (message.kind === 'file') {
    const fileName = message.attachment?.fileName ?? null
    return `📄 文件${fileName ? `：\`${fileName}\`` : ''}`
  }
  return null
}

export function buildRemoteImMessageSummaryMarkdown(
  messages: RemoteImMessage[],
  options: RemoteImMessageSummaryOptions = {}
): string {
  if (!messages.length) {
    return '# 消息记录汇总\n\n_暂无消息记录。_\n'
  }

  const sorted = [...messages].sort(
    (a, b) => a.createdAt - b.createdAt || a.id - b.id
  )
  const groups = new Map<string, RemoteImMessage[]>()
  for (const message of sorted) {
    const peer = peerOf(message)
    const bucket = groups.get(peer)
    if (bucket) bucket.push(message)
    else groups.set(peer, [message])
  }
  // 最近有消息的会话排前面。
  const orderedPeers = [...groups.entries()].sort(
    (a, b) => b[1][b[1].length - 1].createdAt - a[1][a[1].length - 1].createdAt
  )

  const first = sorted[0]
  const last = sorted[sorted.length - 1]
  const lines: string[] = []
  lines.push('# 消息记录汇总')
  lines.push('')
  lines.push(
    `> 📊 **共 ${sorted.length} 条消息 · ${groups.size} 个会话** · 时间范围：${formatSummaryTime(first.createdAt)} ~ ${formatSummaryTime(last.createdAt)}`
  )

  for (const [peer, peerMessages] of orderedPeers) {
    lines.push('')
    lines.push('---')
    lines.push('')
    lines.push(`## 💬 ${peer} · ${peerMessages.length} 条`)
    let lastDay = ''
    for (const message of peerMessages) {
      // 同一会话内按天插入日期分隔，消息头只留「发送者 + 时:分」，更清爽。
      const day = formatSummaryDay(message.createdAt)
      if (day !== lastDay) {
        lastDay = day
        lines.push('')
        lines.push(`### 📅 ${day}`)
      }
      const failed = message.status === 'failed' ? ' ⚠️ 发送失败' : ''
      lines.push('')
      lines.push(`**${senderLabel(message, options.ownerUserId)}** · \`${formatSummaryClock(message.createdAt)}\`${failed}`)
      const attachment = attachmentLine(message)
      if (attachment) {
        lines.push('')
        lines.push(attachment)
      }
      const content = message.content.trim()
      if (content) {
        lines.push('')
        lines.push(content)
      }
    }
  }
  lines.push('')
  return lines.join('\n')
}
