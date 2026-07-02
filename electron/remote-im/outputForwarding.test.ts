import { describe, expect, it } from 'vitest'
import type { CreateRemoteImMessageInput } from './messageStore.js'
import type { RemoteImConfig } from './types.js'
import {
  completeRemoteImOutputSession,
  createRemoteImAicliOutputText,
  createRemoteImOperationFinishedText,
  flushRemoteImOutputSession,
  parseRemoteImAicliOutputText,
  isRemoteImOperationFinishedText,
  type RemoteImOutputFlushTimer,
  type RemoteImOutputSessionState
} from './outputForwarding.js'
import { REMOTE_IM_REPLY_CLOSE_TAG, REMOTE_IM_REPLY_OPEN_TAG } from './replyProtocol.js'

const config: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1400000000,
  desktopUserId: 'controlled_desktop',
  desktopRole: 'slave',
  userSigMode: 'endpoint',
  userSigEndpoint: 'https://example.test/sig',
  userSigSecretKey: '',
  friendUserIds: [],
  masterUserIds: ['master_desktop'],
  slaveUserIds: [],
  allowedUserIds: ['master_desktop'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 4
}

function createState(
  buffer: string,
  configOverrides: Partial<RemoteImConfig> = {}
): RemoteImOutputSessionState {
  return {
    projectId: 'project-1',
    toUserId: 'master_desktop',
    config: { ...config, ...configOverrides },
    buffer,
    timer: null
  }
}

describe('remote IM output forwarding', () => {
  it('flushes buffered output before notifying the master that work completed', () => {
    const state = createState(
      ['terminal noise', REMOTE_IM_REPLY_OPEN_TAG, 'abcdef', REMOTE_IM_REPLY_CLOSE_TAG].join('\n')
    )
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []
    const changedProjects: Array<string | null> = []

    completeRemoteImOutputSession('session-1', state, {
      now: () => 1234,
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      messagesChanged: (projectId) => {
        changedProjects.push(projectId)
      }
    })

    expect(state.buffer).toBe('')
    expect(sentTexts).toEqual([
      createRemoteImAicliOutputText('abcd'),
      createRemoteImAicliOutputText('ef'),
      '操作已完成。'
    ])
    expect(messages.map((message) => message.role)).toEqual(['aicli', 'aicli', 'system'])
    expect(messages.map((message) => message.status)).toEqual([
      'sent-to-im',
      'sent-to-im',
      'sent-to-im'
    ])
    expect(messages[2]).toMatchObject({
      projectId: 'project-1',
      sessionId: 'session-1',
      toUserId: 'master_desktop',
      content: '操作已完成。',
      sentToImAt: 1234
    })
    expect(changedProjects).toEqual(['project-1', 'project-1'])
  })

  it('uses an ended message for abnormal exits', () => {
    expect(createRemoteImOperationFinishedText({ exitCode: 1 })).toBe(
      '操作已结束（退出码：1）。'
    )
    expect(createRemoteImOperationFinishedText({ exitCode: null, signal: 'kill' })).toBe(
      '操作已结束（信号：kill）。'
    )
    expect(isRemoteImOperationFinishedText('操作已完成。')).toBe(true)
    expect(isRemoteImOperationFinishedText('操作已结束（退出码：1）。')).toBe(true)
    expect(isRemoteImOperationFinishedText('检查构建')).toBe(false)
  })

  it('marks forwarded AICLI output so another desktop does not execute it as a command', () => {
    const text = createRemoteImAicliOutputText('build passed')

    expect(parseRemoteImAicliOutputText(text)).toBe('build passed')
    expect(parseRemoteImAicliOutputText('检查构建')).toBeNull()
  })

  it('clears a scheduled flush even when the buffered output is blank', () => {
    const state = createState('\r\n  ')
    const timer = 1 as unknown as RemoteImOutputFlushTimer
    const clearedTimers: RemoteImOutputFlushTimer[] = []
    state.timer = timer

    const chunks = flushRemoteImOutputSession('session-1', state, {
      createMessage: () => undefined,
      sendText: () => undefined,
      messagesChanged: () => undefined,
      clearTimer: (item) => {
        clearedTimers.push(item)
      }
    })

    expect(chunks).toBe(0)
    expect(state.timer).toBeNull()
    expect(clearedTimers).toEqual([timer])
  })

  it('does not forward thinking/status-only terminal redraws', () => {
    const state = createState(
      [
        'thinking with xhigh effort✢thinking with xhigh effort',
        '354 cache 31.7K total in 32.3K/ out 354|ctx 3%/1.0M|5h 4%|7d 22%',
        'Newspapering…✻Cogitated for 28s❯'
      ].join('\n')
    )
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []
    const changedProjects: Array<string | null> = []

    const chunks = flushRemoteImOutputSession('session-1', state, {
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      messagesChanged: (projectId) => {
        changedProjects.push(projectId)
      }
    })

    expect(chunks).toBe(0)
    expect(messages).toEqual([])
    expect(sentTexts).toEqual([])
    expect(changedProjects).toEqual([])
  })

  it('does not forward untagged terminal output', () => {
    const state = createState(
      'Assistant text.│AddedCLAUDE_CODE_DISABLE_MOUSE_CLICKStodisablemouseclick│Discombobulating…✻Cooked for 8s❯'
    )
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []

    const chunks = flushRemoteImOutputSession('session-1', state, {
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      messagesChanged: () => undefined
    })

    expect(chunks).toBe(0)
    expect(state.buffer).toBe('')
    expect(messages).toEqual([])
    expect(sentTexts).toEqual([])
  })

  it('forwards only the assistant reply from prompt echo and TUI redraw noise', () => {
    const state = createState(
      [
        '[IM_REPLY] Put final Markdown for IM between full-line ',
        `${REMOTE_IM_REPLY_OPEN_TAG}\r\n`,
        `and ${REMOTE_IM_REPLY_CLOSE_TAG}; text outside tags is ignored.\r\n`,
        '\u001b[20;1HOpus | 5h:19% 7d:28% | ctx:3%/1M | cache:r15.8k+w2.6k | in:8.7k out:2\u001b[K',
        '\u001b[10;1H⏺\r\n',
        `${REMOTE_IM_REPLY_OPEN_TAG}\r\n`,
        '  我是 Claude Code，Anthropic 出品的命令行编程助手。\r\n',
        '  Opus |\r\n',
        '  5h:19%\r\n',
        '  ctx:3%/1M |\r\n',
        '  cache:r15.8k+w2.6k |\r\n',
        '  ● high · /effort\r\n',
        '  ←\r\n',
        '  for\r\n',
        '  agents\r\n',
        '  - 熟悉多语言代码库的查阅、修改与重构\r\n',
        `  ${REMOTE_IM_REPLY_CLOSE_TAG}\r\n`
      ].join(''),
      { outputMaxChunkChars: 500 }
    )
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []

    const chunks = flushRemoteImOutputSession('session-1', state, {
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      messagesChanged: () => undefined
    })

    const expected = ['我是 Claude Code，Anthropic 出品的命令行编程助手。', '- 熟悉多语言代码库的查阅、修改与重构'].join('\n')
    expect(chunks).toBe(1)
    expect(messages[0]?.content).toBe(expected)
    expect(sentTexts).toEqual([createRemoteImAicliOutputText(expected)])
  })

  it('prefers raw Claude transcript Markdown over terminal-rendered table fragments', () => {
    const terminalRenderedTable = [
      `${REMOTE_IM_REPLY_OPEN_TAG}\r\n`,
      '│ 目录 │ 作用 │ │ chrome/ │ 浏览器主体（UI、标签页、 │\r\n',
      '目录 │ 作用 │ │ 浏览器主体（UI 标签页、扩展） │ │ content/ `third │ │\r\n',
      `${REMOTE_IM_REPLY_CLOSE_TAG}\r\n`
    ].join('')
    const state = createState(terminalRenderedTable, { outputMaxChunkChars: 500 })
    state.transcript = {
      kind: 'claude',
      cwd: '/Users/me/work/repo',
      sinceMs: Date.parse('2026-06-29T00:00:05.000Z')
    }
    const transcriptMarkdown = [
      '## 目录结构',
      '| 目录 | 作用 |',
      '|------|------|',
      '| `chrome/` | 浏览器主体（UI、标签页、扩展） |',
      '| `content/` | 核心渲染引擎 |'
    ].join('\n')
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []

    const chunks = flushRemoteImOutputSession('session-1', state, {
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      messagesChanged: () => undefined,
      readTranscriptReply: () => transcriptMarkdown
    })

    expect(chunks).toBe(1)
    expect(messages[0]?.content).toBe(transcriptMarkdown)
    expect(sentTexts).toEqual([createRemoteImAicliOutputText(transcriptMarkdown)])
    expect(state.buffer).toBe('')
  })

  it('forwards local image paths in AICLI output as image messages', () => {
    const state = createState(
      [
        `${REMOTE_IM_REPLY_OPEN_TAG}\n`,
        '截图如下：![desktop](/Users/me/MultiAICode/remote-im/images/desktop_shot.png)\n',
        `${REMOTE_IM_REPLY_CLOSE_TAG}`
      ].join(''),
      { outputMaxChunkChars: 500 }
    )
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []
    const sentImages: Array<{ projectId: string; toUserId: string; localPath: string }> = []

    const chunks = flushRemoteImOutputSession('session-1', state, {
      now: () => 1234,
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      sendImage: (projectId, toUserId, image) => {
        sentImages.push({ projectId, toUserId, localPath: image.localPath })
      },
      messagesChanged: () => undefined
    })

    expect(chunks).toBe(1)
    expect(sentTexts).toEqual([
      createRemoteImAicliOutputText(
        '截图如下：![desktop](/Users/me/MultiAICode/remote-im/images/desktop_shot.png)'
      )
    ])
    expect(sentImages).toEqual([
      {
        projectId: 'project-1',
        toUserId: 'master_desktop',
        localPath: '/Users/me/MultiAICode/remote-im/images/desktop_shot.png'
      }
    ])
    expect(messages).toHaveLength(2)
    expect(messages[1]).toMatchObject({
      projectId: 'project-1',
      sessionId: 'session-1',
      toUserId: 'master_desktop',
      content: '[图片消息] desktop_shot.png',
      kind: 'image',
      attachment: {
        type: 'image',
        localPath: '/Users/me/MultiAICode/remote-im/images/desktop_shot.png',
        fileName: 'desktop_shot.png',
        mimeType: 'image/png'
      },
      status: 'sent-to-im',
      sentToImAt: 1234
    })
  })

  it('extracts image paths from the full AICLI output before text chunking', () => {
    const localPath = '/Users/me/MultiAICode/remote-im/images/desktop_shot.png'
    const state = createState(
      [
        `${REMOTE_IM_REPLY_OPEN_TAG}\n`,
        `截图如下：![desktop](${localPath})\n`,
        `${REMOTE_IM_REPLY_CLOSE_TAG}`
      ].join(''),
      { outputMaxChunkChars: 20 }
    )
    const sentImages: string[] = []

    const chunks = flushRemoteImOutputSession('session-1', state, {
      createMessage: () => undefined,
      sendText: () => undefined,
      sendImage: (_projectId, _toUserId, image) => {
        sentImages.push(image.localPath)
      },
      messagesChanged: () => undefined
    })

    expect(chunks).toBeGreaterThan(1)
    expect(sentImages).toEqual([localPath])
  })

  it('keeps an incomplete tagged reply buffered until the close tag arrives', () => {
    const state = createState(`noise\n${REMOTE_IM_REPLY_OPEN_TAG}\nhello`, {
      outputMaxChunkChars: 100
    })
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []

    const firstFlush = flushRemoteImOutputSession('session-1', state, {
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      messagesChanged: () => undefined
    })

    expect(firstFlush).toBe(0)
    expect(state.buffer).toBe(`${REMOTE_IM_REPLY_OPEN_TAG}\nhello`)
    expect(messages).toEqual([])
    expect(sentTexts).toEqual([])

    state.buffer += `\n${REMOTE_IM_REPLY_CLOSE_TAG}`
    const secondFlush = flushRemoteImOutputSession('session-1', state, {
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      messagesChanged: () => undefined
    })

    expect(secondFlush).toBe(1)
    expect(state.buffer).toBe('')
    expect(messages[0]?.content).toBe('hello')
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('hello')])
  })
})
