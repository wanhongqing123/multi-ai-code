import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it, vi } from 'vitest'
import type { CreateRemoteImMessageInput } from '../../../electron/remote-im/messageStore.js'
import type { RemoteImConfig } from '../../../electron/remote-im/types.js'
import {
  completeRemoteImOutputSession,
  createRemoteImAicliOutputText,
  createRemoteImOperationFailedText,
  createRemoteImOperationFinishedText,
  failRemoteImOutputSession,
  forwardRemoteImStructuredAssistantOutput,
  forwardRemoteImStructuredFinalOutput,
  flushRemoteImOutputSession,
  parseRemoteImAicliOutputText,
  resolveRemoteImStructuredFinalContent,
  isRemoteImOperationFinishedText,
  type RemoteImOutputForwardingDeps,
  type RemoteImOutputFlushTimer,
  type RemoteImOutputSessionState
} from '../../../electron/remote-im/outputForwarding.js'
import {
  REMOTE_IM_REPLY_CLOSE_TAG,
  REMOTE_IM_REPLY_OPEN_TAG
} from '../../../electron/remote-im/replyProtocol.js'

const __dirname = dirname(fileURLToPath(import.meta.url))

function readReplyFixture(name: string): string {
  return readFileSync(join(__dirname, 'fixtures', 'reply-protocol', name), 'utf8')
}

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
    expect(createRemoteImOperationFinishedText({ exitCode: 1 })).toBe('操作已结束（退出码：1）。')
    expect(createRemoteImOperationFinishedText({ exitCode: null, signal: 'kill' })).toBe(
      '操作已结束（信号：kill）。'
    )
    expect(isRemoteImOperationFinishedText('操作已完成。')).toBe(true)
    expect(isRemoteImOperationFinishedText('操作已结束（退出码：1）。')).toBe(true)
    expect(isRemoteImOperationFinishedText('AICLI 执行失败：network disconnected')).toBe(true)
    expect(isRemoteImOperationFinishedText('检查构建')).toBe(false)
  })

  it('flushes a completed reply before forwarding a terminal AICLI error', () => {
    const state = createState(
      [REMOTE_IM_REPLY_OPEN_TAG, 'partial result', REMOTE_IM_REPLY_CLOSE_TAG].join('\n'),
      { outputMaxChunkChars: 500 }
    )
    const timer = 1 as unknown as RemoteImOutputFlushTimer
    state.timer = timer
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []
    const clearedTimers: RemoteImOutputFlushTimer[] = []

    failRemoteImOutputSession(
      'session-1',
      state,
      {
        now: () => 1234,
        createMessage: (input) => messages.push(input),
        sendText: (_projectId, _toUserId, text) => sentTexts.push(text),
        messagesChanged: () => undefined,
        clearTimer: (item) => clearedTimers.push(item)
      },
      ' stream disconnected '
    )

    expect(state.buffer).toBe('')
    expect(state.timer).toBeNull()
    expect(clearedTimers).toEqual([timer])
    expect(sentTexts).toEqual([
      createRemoteImAicliOutputText('partial result'),
      'AICLI 执行失败：stream disconnected'
    ])
    expect(messages.map((message) => message.role)).toEqual(['aicli', 'system'])
    expect(messages[1]).toMatchObject({
      content: createRemoteImOperationFailedText('stream disconnected'),
      sentToImAt: 1234
    })
  })

  it('marks forwarded AICLI output so another desktop does not execute it as a command', () => {
    const text = createRemoteImAicliOutputText('build passed')

    expect(parseRemoteImAicliOutputText(text)).toBe('build passed')
    expect(text).not.toContain('AICLI 输出')
    expect(parseRemoteImAicliOutputText('【AICLI 输出】\nbuild passed')).toBe('build passed')
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

    const expected = [
      '我是 Claude Code，Anthropic 出品的命令行编程助手。',
      '- 熟悉多语言代码库的查阅、修改与重构'
    ].join('\n')
    expect(chunks).toBe(1)
    expect(messages[0]?.content).toBe(expected)
    expect(sentTexts).toEqual([createRemoteImAicliOutputText(expected)])
  })

  it('forwards only tagged output for the current reply id', () => {
    const state = createState(
      [
        '<remote-im-reply id="old-reply">',
        'old result',
        '</remote-im-reply id="old-reply">',
        REMOTE_IM_REPLY_OPEN_TAG,
        'legacy result',
        REMOTE_IM_REPLY_CLOSE_TAG,
        '<remote-im-reply id="reply-current">',
        'current result',
        '</remote-im-reply id="reply-current">'
      ].join('\n'),
      { outputMaxChunkChars: 500 }
    )
    state.replyId = 'reply-current'
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

    expect(chunks).toBe(1)
    expect(messages[0]?.content).toBe('current result')
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('current result')])
  })

  it('forwards a Codex inline reply without leaking malformed markers', () => {
    const state = createState(
      [
        '<remote-im-reply id="rim-0123456789abcdef你好，有什么需要我帮忙的？',
        '</remote-im-reply id="rim-0123456789abcdef'
      ].join('\n'),
      { outputMaxChunkChars: 500 }
    )
    state.replyId = 'rim-0123456789abcdef'
    state.sourceKind = 'codex'
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []

    const chunks = flushRemoteImOutputSession('session-1', state, {
      createMessage: (input) => messages.push(input),
      sendText: (_projectId, _toUserId, text) => sentTexts.push(text),
      messagesChanged: () => undefined
    })

    expect(chunks).toBe(1)
    expect(messages[0]?.content).toBe('你好，有什么需要我帮忙的？')
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('你好，有什么需要我帮忙的？')])
  })

  it('does not forward the same reply id more than once', () => {
    const taggedReply = [
      '<remote-im-reply id="reply-current">',
      'current result',
      '</remote-im-reply id="reply-current">'
    ].join('\n')
    const state = createState(taggedReply, { outputMaxChunkChars: 500 })
    state.replyId = 'reply-current'
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []

    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      messagesChanged: () => undefined
    }

    expect(flushRemoteImOutputSession('session-1', state, deps)).toBe(1)
    state.buffer += taggedReply
    expect(flushRemoteImOutputSession('session-1', state, deps)).toBe(0)

    expect(messages).toHaveLength(1)
    expect(messages[0]?.content).toBe('current result')
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('current result')])
  })

  it('forwards Claude output when a matching id open tag is closed by a legacy close tag', () => {
    const state = createState(readReplyFixture('claude-id-open-legacy-close.txt'), {
      outputMaxChunkChars: 500
    })
    state.replyId = 'rim-current'
    state.sourceKind = 'claude'
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

    const expected =
      'Claude transcript reply with an id-bearing opening marker and a legacy closing marker.'
    expect(chunks).toBe(1)
    expect(messages[0]?.content).toBe(expected)
    expect(sentTexts).toEqual([createRemoteImAicliOutputText(expected)])
  })

  it('replays a Codex current reply fixture with TUI noise before forwarding', () => {
    const state = createState(readReplyFixture('codex-current-reply-with-tui-noise.txt'), {
      outputMaxChunkChars: 500
    })
    state.replyId = 'rim-current'
    state.sourceKind = 'codex'
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

    const expected = 'Codex reply after terminal UI redraw noise.'
    expect(chunks).toBe(1)
    expect(messages[0]?.content).toBe(expected)
    expect(sentTexts).toEqual([createRemoteImAicliOutputText(expected)])
  })

  it('forwards OpenCode tagged replies through the normal terminal buffer path', () => {
    const state = createState(
      [
        'opencode terminal chrome outside tags',
        '<remote-im-reply id="rim-current">',
        'OpenCode reply for IM.',
        '</remote-im-reply id="rim-current">'
      ].join('\n'),
      { outputMaxChunkChars: 500 }
    )
    state.replyId = 'rim-current'
    state.sourceKind = 'opencode'
    state.transcript = {
      kind: 'opencode' as 'claude',
      cwd: '/Users/me/work/repo',
      sinceMs: Date.parse('2026-07-09T00:00:00.000Z'),
      replyId: 'rim-current'
    }
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
      readTranscriptReply: () => {
        throw new Error('OpenCode should not read transcript storage')
      }
    })

    expect(chunks).toBe(1)
    expect(messages[0]?.content).toBe('OpenCode reply for IM.')
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('OpenCode reply for IM.')])
  })

  it('forwards a markerless source-confirmed final reply without intermediate output', () => {
    const state = createState('intermediate text without markers', {
      outputMaxChunkChars: 500
    })
    state.replyId = 'rim-current'
    state.sourceKind = 'opencode'
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => messages.push(input),
      sendText: (_projectId, _toUserId, text) => sentTexts.push(text),
      messagesChanged: () => undefined
    }

    expect(flushRemoteImOutputSession('session-1', state, deps)).toBe(0)
    expect(
      forwardRemoteImStructuredFinalOutput(
        'session-1',
        state,
        deps,
        '最终结论：使用源码终态事件转发。'
      )
    ).toBe(1)

    expect(messages.map((message) => message.content)).toEqual(['最终结论：使用源码终态事件转发。'])
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('最终结论：使用源码终态事件转发。')])
  })

  it('forwards every source-authored assistant message immediately', () => {
    const state = createState('', { outputMaxChunkChars: 500 })
    state.replyId = 'rim-current'
    state.taskId = 'task-current'
    state.sourceKind = 'codex'
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => messages.push(input),
      sendText: (_projectId, _toUserId, text) => sentTexts.push(text),
      messagesChanged: () => undefined
    }

    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '仍在编译，预计还需要一段时间。',
        'message-1',
        'part-1'
      )
    ).toBe(1)
    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '安装包正在上传。',
        'message-2',
        'part-2'
      )
    ).toBe(1)
    expect(
      forwardRemoteImStructuredFinalOutput(
        'session-1',
        state,
        deps,
        '编译完成并已发布。',
        'message-final'
      )
    ).toBe(1)

    expect(messages.map((message) => message.content)).toEqual([
      '仍在编译，预计还需要一段时间。',
      '安装包正在上传。',
      '编译完成并已发布。'
    ])
    expect(sentTexts).toEqual([
      createRemoteImAicliOutputText('仍在编译，预计还需要一段时间。'),
      createRemoteImAicliOutputText('安装包正在上传。'),
      createRemoteImAicliOutputText('编译完成并已发布。')
    ])
  })

  it('stays silent for empty and noise-only assistant events', () => {
    const state = createState('', { outputMaxChunkChars: 500 })
    state.replyId = 'rim-current'
    state.taskId = 'task-current'
    state.sourceKind = 'codex'
    const messages: CreateRemoteImMessageInput[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => messages.push(input),
      sendText: () => undefined,
      messagesChanged: () => undefined
    }

    expect(
      forwardRemoteImStructuredAssistantOutput('session-1', state, deps, '', 'message-empty')
    ).toBe(0)
    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        'Use /skills to list available skills.',
        'message-noise'
      )
    ).toBe(0)
    expect(messages).toEqual([])
  })

  it('deduplicates reliable retransmission by assistant event identity', () => {
    const state = createState('', { outputMaxChunkChars: 500 })
    state.replyId = 'rim-current'
    state.taskId = 'task-current'
    state.sourceKind = 'opencode'
    const messages: CreateRemoteImMessageInput[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => messages.push(input),
      sendText: () => undefined,
      messagesChanged: () => undefined
    }

    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '正在上传安装包。',
        'message-1',
        'part-1'
      )
    ).toBe(1)
    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '正在上传安装包。',
        'message-1',
        'part-1'
      )
    ).toBe(0)
    expect(messages.map((message) => message.content)).toEqual(['正在上传安装包。'])
  })

  it('allows identical text from distinct assistant events', () => {
    const state = createState('', { outputMaxChunkChars: 500 })
    state.replyId = 'rim-current'
    state.taskId = 'task-current'
    state.sourceKind = 'opencode'
    const messages: CreateRemoteImMessageInput[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => messages.push(input),
      sendText: () => undefined,
      messagesChanged: () => undefined
    }

    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '相同但独立的回复。',
        'message-1',
        'part-1'
      )
    ).toBe(1)
    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '相同但独立的回复。',
        'message-2',
        'part-2'
      )
    ).toBe(1)
    expect(messages.map((message) => message.content)).toEqual([
      '相同但独立的回复。',
      '相同但独立的回复。'
    ])
  })

  it('does not resend an OpenCode final aggregate already emitted as assistant text', () => {
    const state = createState('', { outputMaxChunkChars: 500 })
    state.sourceKind = 'opencode'
    const messages: CreateRemoteImMessageInput[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => messages.push(input),
      sendText: () => undefined,
      messagesChanged: () => undefined
    }

    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '第一段',
        'message-1:part-1',
        'part-1'
      )
    ).toBe(1)
    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '第二段',
        'message-1:part-2',
        'part-2'
      )
    ).toBe(1)
    expect(
      forwardRemoteImStructuredFinalOutput(
        'session-1',
        state,
        deps,
        '第一段\n第二段',
        'message-1:final'
      )
    ).toBe(0)
    expect(messages.map((message) => message.content)).toEqual(['第一段', '第二段'])
  })

  it('forwards a distinct final after immediate assistant output', () => {
    const state = createState('', { outputMaxChunkChars: 500 })
    state.sourceKind = 'codex'
    const messages: CreateRemoteImMessageInput[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => messages.push(input),
      sendText: () => undefined,
      messagesChanged: () => undefined
    }

    expect(
      forwardRemoteImStructuredAssistantOutput(
        'session-1',
        state,
        deps,
        '我先检查构建日志。',
        'message-commentary'
      )
    ).toBe(1)
    expect(
      forwardRemoteImStructuredFinalOutput(
        'session-1',
        state,
        deps,
        '构建完成，测试通过。',
        'message-final'
      )
    ).toBe(1)
    expect(messages.map((message) => message.content)).toEqual([
      '我先检查构建日志。',
      '构建完成，测试通过。'
    ])
  })

  it('deduplicates retransmission of the same structured terminal event', () => {
    const tagged = [
      '<remote-im-reply id="rim-current">',
      'marker reply',
      '</remote-im-reply id="rim-current">'
    ].join('\n')
    const state = createState(tagged, { outputMaxChunkChars: 500 })
    state.replyId = 'rim-current'
    state.sourceKind = 'codex'
    const sentTexts: string[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: () => undefined,
      sendText: (_projectId, _toUserId, text) => sentTexts.push(text),
      messagesChanged: () => undefined
    }

    expect(
      forwardRemoteImStructuredFinalOutput('session-1', state, deps, tagged, 'event-final-1')
    ).toBe(1)
    expect(
      forwardRemoteImStructuredFinalOutput(
        'session-1',
        state,
        deps,
        'marker reply',
        'event-final-1'
      )
    ).toBe(0)
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('marker reply')])
  })

  it('does not use reply id as structured terminal event deduplication key', () => {
    const state = createState('', { outputMaxChunkChars: 500 })
    state.replyId = 'rim-current'
    state.sourceKind = 'opencode'
    const sentTexts: string[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: () => undefined,
      sendText: (_projectId, _toUserId, text) => sentTexts.push(text),
      messagesChanged: () => undefined
    }

    expect(
      forwardRemoteImStructuredFinalOutput(
        'session-1',
        state,
        deps,
        'first terminal event',
        'event-final-1'
      )
    ).toBe(1)
    expect(
      forwardRemoteImStructuredFinalOutput(
        'session-1',
        state,
        deps,
        'later terminal event',
        'event-final-2'
      )
    ).toBe(1)
    expect(sentTexts).toEqual([
      createRemoteImAicliOutputText('first terminal event'),
      createRemoteImAicliOutputText('later terminal event')
    ])
  })

  it('forwards a source-confirmed final reply even when its text has a stale reply marker', () => {
    const state = createState('', { outputMaxChunkChars: 500 })
    state.replyId = 'rim-current'
    state.sourceKind = 'opencode'
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []
    const tagged = [
      '<remote-im-reply id="rim-other">',
      'reply for another request',
      '</remote-im-reply id="rim-other">'
    ].join('\n')

    expect(
      forwardRemoteImStructuredFinalOutput(
        'session-1',
        state,
        {
          createMessage: (input) => messages.push(input),
          sendText: (_projectId, _toUserId, text) => sentTexts.push(text),
          messagesChanged: () => undefined
        },
        tagged
      )
    ).toBe(1)

    expect(messages.map((message) => message.content)).toEqual(['reply for another request'])
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('reply for another request')])
    expect(resolveRemoteImStructuredFinalContent(tagged, 'rim-current')).toEqual({
      content: 'reply for another request',
      source: 'fallback-marker',
      markerReplyIdMismatch: true
    })
  })

  it('forwards only the latest stale marker body from a structured final event', () => {
    const tagged = [
      '<remote-im-reply id="rim-old-1">',
      'older progress',
      '</remote-im-reply id="rim-old-1">',
      '<remote-im-reply id="rim-old-2">',
      'latest final result',
      '</remote-im-reply id="rim-old-2">'
    ].join('\n')

    expect(resolveRemoteImStructuredFinalContent(tagged, 'rim-current')).toEqual({
      content: 'latest final result',
      source: 'fallback-marker',
      markerReplyIdMismatch: true
    })
  })

  it('forwards a completed terminal reply once after an incomplete marker was buffered', () => {
    const state = createState(
      ['terminal noise', '<remote-im-reply id="rim-current">', 'partial'].join('\n'),
      { outputMaxChunkChars: 500 }
    )
    state.replyId = 'rim-current'
    state.sourceKind = 'codex'
    const messages: CreateRemoteImMessageInput[] = []
    const sentTexts: string[] = []
    const deps: RemoteImOutputForwardingDeps = {
      createMessage: (input) => messages.push(input),
      sendText: (_projectId, _toUserId, text) => sentTexts.push(text),
      messagesChanged: () => undefined
    }
    const complete = [
      '<remote-im-reply id="rim-current">',
      'complete terminal reply',
      '</remote-im-reply id="rim-current">'
    ].join('\n')

    expect(
      forwardRemoteImStructuredFinalOutput('session-1', state, deps, complete, 'event-final-1')
    ).toBe(1)
    expect(
      forwardRemoteImStructuredFinalOutput('session-1', state, deps, complete, 'event-final-1')
    ).toBe(0)
    expect(messages.map((message) => message.content)).toEqual(['complete terminal reply'])
    expect(sentTexts).toEqual([createRemoteImAicliOutputText('complete terminal reply')])
  })

  it('prefers raw Claude transcript Markdown over terminal-rendered table fragments', () => {
    const terminalRenderedTable = [
      `${REMOTE_IM_REPLY_OPEN_TAG}\r\n`,
      '│ 目录 │ 作用 │ │ chrome/ │ 浏览器主体（UI、标签页、 │\r\n',
      '目录 │ 作用 │ │ 浏览器主体（UI 标签页、扩展） │ │ content/ `third │ │\r\n',
      `${REMOTE_IM_REPLY_CLOSE_TAG}\r\n`
    ].join('')
    const state = createState(terminalRenderedTable, {
      outputMaxChunkChars: 500
    })
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

  it('does not infer image messages from AICLI text output', () => {
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
    const sendImage = vi.fn()

    const deps: RemoteImOutputForwardingDeps & { sendImage: typeof sendImage } = {
      now: () => 1234,
      createMessage: (input) => {
        messages.push(input)
      },
      sendText: (_projectId, _toUserId, text) => {
        sentTexts.push(text)
      },
      sendImage,
      messagesChanged: () => undefined
    }

    const chunks = flushRemoteImOutputSession('session-1', state, deps)

    expect(chunks).toBe(1)
    expect(sentTexts).toEqual([
      createRemoteImAicliOutputText(
        '截图如下：![desktop](/Users/me/MultiAICode/remote-im/images/desktop_shot.png)'
      )
    ])
    expect(messages).toEqual([
      expect.objectContaining({
        projectId: 'project-1',
        sessionId: 'session-1',
        toUserId: 'master_desktop',
        content: '截图如下：![desktop](/Users/me/MultiAICode/remote-im/images/desktop_shot.png)',
        status: 'sent-to-im',
        sentToImAt: 1234
      })
    ])
    expect(messages[0]).not.toHaveProperty('kind')
    expect(sendImage).not.toHaveBeenCalled()
  })

  it('keeps image-looking AICLI output as text when chunking', () => {
    const state = createState(
      [
        `${REMOTE_IM_REPLY_OPEN_TAG}\n`,
        '截图如下：![desktop](/Users/me/MultiAICode/remote-im/images/desktop_shot.png)\n',
        `${REMOTE_IM_REPLY_CLOSE_TAG}`
      ].join(''),
      { outputMaxChunkChars: 20 }
    )
    const sendImage = vi.fn()

    const deps: RemoteImOutputForwardingDeps & { sendImage: typeof sendImage } = {
      createMessage: () => undefined,
      sendText: () => undefined,
      sendImage,
      messagesChanged: () => undefined
    }

    const chunks = flushRemoteImOutputSession('session-1', state, deps)

    expect(chunks).toBeGreaterThan(1)
    expect(sendImage).not.toHaveBeenCalled()
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
