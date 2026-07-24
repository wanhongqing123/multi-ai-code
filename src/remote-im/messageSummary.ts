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

export function summarySenderLabel(message: RemoteImMessage, ownerUserId?: string | null): string {
  if (message.direction === 'incoming') return message.fromUserId?.trim() || '对方'
  if (message.direction === 'outgoing') return ownerUserId?.trim() || '我'
  return message.role === 'aicli' ? 'AICLI' : '系统'
}

export interface RemoteImSummaryAttachmentParts {
  icon: '📷' | '📄'
  kindLabel: '图片' | '文件'
  fileName: string | null
}

export function summaryAttachmentParts(message: RemoteImMessage): RemoteImSummaryAttachmentParts | null {
  if (message.kind === 'image') {
    return { icon: '📷', kindLabel: '图片', fileName: message.attachment?.fileName ?? null }
  }
  if (message.kind === 'file') {
    return { icon: '📄', kindLabel: '文件', fileName: message.attachment?.fileName ?? null }
  }
  return null
}

function attachmentLine(message: RemoteImMessage): string | null {
  const parts = summaryAttachmentParts(message)
  if (!parts) return null
  return `${parts.icon} ${parts.kindLabel}${parts.fileName ? `：\`${parts.fileName}\`` : ''}`
}

export interface RemoteImSummaryGroup {
  peer: string
  messages: RemoteImMessage[]
}

export interface RemoteImMessageSummaryData {
  total: number
  sessionCount: number
  firstAt: number
  lastAt: number
  groups: RemoteImSummaryGroup[]
}

// 汇总的共享结构：按会话分组（最近活跃在前）、组内按时间升序。
// Markdown 生成与弹窗的结构化展示共用同一份分组逻辑。
export function summarizeRemoteImMessages(messages: RemoteImMessage[]): RemoteImMessageSummaryData | null {
  if (!messages.length) return null
  const sorted = [...messages].sort((a, b) => a.createdAt - b.createdAt || a.id - b.id)
  const groups = new Map<string, RemoteImMessage[]>()
  for (const message of sorted) {
    const peer = peerOf(message)
    const bucket = groups.get(peer)
    if (bucket) bucket.push(message)
    else groups.set(peer, [message])
  }
  const orderedGroups = [...groups.entries()]
    .sort((a, b) => b[1][b[1].length - 1].createdAt - a[1][a[1].length - 1].createdAt)
    .map(([peer, peerMessages]) => ({ peer, messages: peerMessages }))
  return {
    total: sorted.length,
    sessionCount: orderedGroups.length,
    firstAt: sorted[0].createdAt,
    lastAt: sorted[sorted.length - 1].createdAt,
    groups: orderedGroups
  }
}

export function buildRemoteImMessageSummaryMarkdown(
  messages: RemoteImMessage[],
  options: RemoteImMessageSummaryOptions = {}
): string {
  const summary = summarizeRemoteImMessages(messages)
  if (!summary) {
    return '# 消息记录汇总\n\n_暂无消息记录。_\n'
  }

  const lines: string[] = []
  lines.push('# 消息记录汇总')
  lines.push('')
  lines.push(
    `> 📊 **共 ${summary.total} 条消息 · ${summary.sessionCount} 个会话** · 时间范围：${formatSummaryTime(summary.firstAt)} ~ ${formatSummaryTime(summary.lastAt)}`
  )

  for (const { peer, messages: peerMessages } of summary.groups) {
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
      lines.push(`**${summarySenderLabel(message, options.ownerUserId)}** · \`${formatSummaryClock(message.createdAt)}\`${failed}`)
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
