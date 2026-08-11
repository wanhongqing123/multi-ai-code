import { describe, expect, it } from 'vitest'
import type { RemoteImMessage } from '../../../electron/preload.js'
import {
  buildRemoteImMessageSummaryMarkdown,
  formatSummaryClock,
  formatSummaryDay,
  formatSummaryTime,
  summaryMessageContent
} from '../../../src/remote-im/messageSummary.js'

let nextId = 1

function makeMessage(input: Partial<RemoteImMessage>): RemoteImMessage {
  return {
    id: nextId++,
    projectId: 'p1',
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: null,
    toUserId: null,
    role: 'remote-user',
    direction: 'incoming',
    content: '',
    kind: 'text',
    attachment: null,
    status: 'received',
    error: null,
    createdAt: Date.UTC(2026, 6, 24, 4, 0, 0),
    sentToAicliAt: null,
    sentToImAt: null,
    ...input
  } as RemoteImMessage
}

describe('buildRemoteImMessageSummaryMarkdown', () => {
  it('renders an empty placeholder when there are no messages', () => {
    const markdown = buildRemoteImMessageSummaryMarkdown([])
    expect(markdown).toContain('# 消息记录汇总')
    expect(markdown).toContain('暂无消息记录')
  })

  it('filters system receipts out of the summary', () => {
    const messages = [
      makeMessage({
        fromUserId: 'whq-iphone',
        direction: 'incoming',
        content: '你好'
      }),
      makeMessage({
        toUserId: 'whq-iphone',
        direction: 'outgoing',
        role: 'system',
        content: '已发送给当前 AICLI，开始处理。'
      })
    ]

    const markdown = buildRemoteImMessageSummaryMarkdown(messages)

    expect(markdown).toContain('**共 1 条消息 · 1 个会话**')
    expect(markdown).not.toContain('已发送给当前 AICLI')
  })

  it('groups messages by peer with stats and chronological order', () => {
    const t1 = Date.UTC(2026, 6, 23, 2, 0, 0)
    const t2 = Date.UTC(2026, 6, 24, 2, 0, 0)
    const t3 = Date.UTC(2026, 6, 24, 3, 0, 0)
    const messages = [
      makeMessage({
        fromUserId: 'whq-iphone',
        direction: 'incoming',
        content: '执行的如何了？',
        createdAt: t2
      }),
      makeMessage({
        toUserId: 'whq-iphone',
        direction: 'outgoing',
        role: 'aicli',
        content: '任务已完成。',
        createdAt: t3
      }),
      makeMessage({
        fromUserId: 'mac-quarkpc',
        direction: 'incoming',
        content: '你好',
        createdAt: t1
      })
    ]

    const markdown = buildRemoteImMessageSummaryMarkdown(messages, { ownerUserId: 'house-pc' })

    expect(markdown).toContain('**共 3 条消息 · 2 个会话**')
    expect(markdown).toContain(`时间范围：${formatSummaryTime(t1)} ~ ${formatSummaryTime(t3)}`)
    expect(markdown).toContain('## 💬 whq-iphone · 2 条')
    expect(markdown).toContain('## 💬 mac-quarkpc · 1 条')
    // 最近活跃的会话排前面。
    expect(markdown.indexOf('## 💬 whq-iphone')).toBeLessThan(markdown.indexOf('## 💬 mac-quarkpc'))
    // 会话内按天插入日期分隔。
    expect(markdown).toContain(`### 📅 ${formatSummaryDay(t2)}`)
    expect(markdown).toContain(`### 📅 ${formatSummaryDay(t1)}`)
    // 入站显示对端名，出站显示本机账号名；消息头带完整「日期 时:分」，
    // 这样单条被摘录出去时也能确定时间，不必依赖上方的日期分隔行。
    expect(markdown).toContain(`**whq-iphone** · \`${formatSummaryTime(t2)}\``)
    expect(markdown).toContain(`**house-pc** · \`${formatSummaryTime(t3)}\``)
    expect(markdown).toContain(`\`${formatSummaryDay(t2)} ${formatSummaryClock(t2)}\``)
    expect(markdown).toContain('执行的如何了？')
    expect(markdown).toContain('任务已完成。')
  })

  it('marks failed messages and renders attachment lines', () => {
    const messages = [
      makeMessage({
        fromUserId: 'whq-iphone',
        direction: 'incoming',
        kind: 'image',
        content: '',
        attachment: {
          type: 'image',
          localPath: null,
          remoteUrl: null,
          thumbnailUrl: null,
          width: null,
          height: null,
          sizeBytes: null,
          fileName: 'shot.png',
          mimeType: 'image/png',
          sdkImageId: null
        }
      }),
      makeMessage({
        toUserId: 'whq-iphone',
        direction: 'outgoing',
        kind: 'file',
        status: 'failed',
        content: '周报',
        attachment: {
          type: 'file',
          localPath: null,
          remoteUrl: null,
          sizeBytes: null,
          fileName: 'weekly.md',
          mimeType: 'text/markdown',
          sdkFileId: null
        }
      })
    ]

    const markdown = buildRemoteImMessageSummaryMarkdown(messages)

    expect(markdown).toContain('📷 图片：`shot.png`')
    expect(markdown).toContain('📄 文件：`weekly.md`')
    expect(markdown).toContain('⚠️ 发送失败')
    // 出站无 ownerUserId 时退化为「我」。
    expect(markdown).toContain('**我** ·')
  })

  it('keeps local image references and removes generated image placeholders', () => {
    const imagePath = '/Users/test/remote-im/images/project-1/shot.png'
    const message = makeMessage({
      fromUserId: 'whq-iphone',
      direction: 'incoming',
      kind: 'image',
      content: '[图片消息] shot.png\n请查看这张截图',
      attachment: {
        type: 'image',
        localPath: imagePath,
        remoteUrl: null,
        thumbnailUrl: null,
        width: 1290,
        height: 2796,
        sizeBytes: 4096,
        fileName: 'shot.png',
        mimeType: 'image/png',
        sdkImageId: null
      }
    })

    const markdown = buildRemoteImMessageSummaryMarkdown([message])

    expect(markdown).toContain(`![shot.png](<${imagePath}>)`)
    expect(markdown).toContain('请查看这张截图')
    expect(markdown).not.toContain('[图片消息] shot.png')
    expect(summaryMessageContent(message)).toBe('请查看这张截图')
  })

  it('sanitizes image metadata before embedding it in summary Markdown', () => {
    const message = makeMessage({
      fromUserId: 'whq-iphone',
      direction: 'incoming',
      kind: 'image',
      content: '[图片消息]',
      attachment: {
        type: 'image',
        localPath: null,
        remoteUrl: 'javascript:alert(1)',
        thumbnailUrl: null,
        width: null,
        height: null,
        sizeBytes: null,
        fileName: 'shot`\n## injected.png',
        mimeType: 'image/png',
        sdkImageId: null
      }
    })

    const markdown = buildRemoteImMessageSummaryMarkdown([message])

    expect(markdown).toContain("📷 图片：`shot' ## injected.png`")
    expect(markdown).not.toContain('javascript:')
    expect(markdown).not.toContain('\n## injected.png')
  })
})
