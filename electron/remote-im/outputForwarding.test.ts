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
