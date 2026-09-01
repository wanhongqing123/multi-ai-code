import { mkdtempSync, mkdirSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import {
  getClaudeProjectTranscriptDir,
  readLatestClaudeRemoteImReply
} from '../../../electron/remote-im/claudeTranscript.js'
import { buildRemoteImAicliPrompt } from '../../../electron/remote-im/replyProtocol.js'

function transcriptUser(replyId: string, timestamp: string, text = 'continue') {
  return {
    type: 'user',
    timestamp,
    message: {
      role: 'user',
      content: buildRemoteImAicliPrompt(
        { fromUserId: 'phone', text, replyId },
        { includeReplyProtocol: true }
      )
    }
  }
}

function transcriptAssistant(replyId: string, timestamp: string, content: string, uuid: string) {
  return {
    type: 'assistant',
    uuid,
    timestamp,
    message: {
      role: 'assistant',
      content: [
        {
          type: 'text',
          text: [
            `<remote-im-reply id="${replyId}">`,
            content,
            `</remote-im-reply id="${replyId}">`
          ].join('\n')
        }
      ]
    }
  }
}

describe('Claude transcript remote IM replies', () => {
  it('maps a cwd to Claude Code project transcript directory', () => {
    expect(getClaudeProjectTranscriptDir('/Users/me/work/repo', '/tmp/claude-projects')).toBe(
      '/tmp/claude-projects/-Users-me-work-repo'
    )
  })

  it('reads the raw tagged Markdown reply instead of terminal-rendered table output', () => {
    const root = mkdtempSync(join(tmpdir(), 'multi-ai-code-claude-transcript-'))
    const cwd = '/Users/me/work/repo'
    const dir = getClaudeProjectTranscriptDir(cwd, root)
    mkdirSync(dir, { recursive: true })
    const transcript = join(dir, 'session.jsonl')
    const rawReply = [
      '<remote-im-reply>',
      '## 目录结构',
      '| 目录 | 作用 |',
      '|------|------|',
      '| `chrome/` | 浏览器主体 |',
      '| `content/` | 渲染引擎 |',
      '</remote-im-reply>'
    ].join('\n')

    writeFileSync(
      transcript,
      [
        JSON.stringify({
          type: 'assistant',
          timestamp: '2026-06-29T00:00:00.000Z',
          message: {
            role: 'assistant',
            content: [{ type: 'text', text: '<remote-im-reply>old</remote-im-reply>' }]
          }
        }),
        JSON.stringify({
          type: 'assistant',
          timestamp: '2026-06-29T00:00:10.000Z',
          message: {
            role: 'assistant',
            content: [{ type: 'text', text: rawReply }]
          }
        })
      ].join('\n') + '\n',
      'utf8'
    )

    expect(
      readLatestClaudeRemoteImReply({
        cwd,
        projectsRoot: root,
        sinceMs: Date.parse('2026-06-29T00:00:05.000Z')
      })
    ).toEqual({
      content: ['## 目录结构', '| 目录 | 作用 |', '|------|------|', '| `chrome/` | 浏览器主体 |', '| `content/` | 渲染引擎 |'].join(
        '\n'
      ),
      completed: true,
      completedReplyIds: [],
      frameId: 'session.jsonl:1'
    })
  })

  it('reads only the transcript reply matching the expected reply id', () => {
    const root = mkdtempSync(join(tmpdir(), 'multi-ai-code-claude-transcript-'))
    const cwd = '/Users/me/work/repo'
    const dir = getClaudeProjectTranscriptDir(cwd, root)
    mkdirSync(dir, { recursive: true })
    const transcript = join(dir, 'session.jsonl')

    writeFileSync(
      transcript,
      [
        JSON.stringify(transcriptUser('reply-current', '2026-06-29T00:00:06.000Z')),
        JSON.stringify({
          type: 'assistant',
          uuid: 'assistant-current',
          timestamp: '2026-06-29T00:00:10.000Z',
          message: {
            role: 'assistant',
            content: [
              {
                type: 'text',
                text: [
                  '<remote-im-reply id="reply-current">',
                  'current transcript reply',
                  '</remote-im-reply id="reply-current">'
                ].join('\n')
              }
            ]
          }
        }),
        JSON.stringify({
          type: 'assistant',
          timestamp: '2026-06-29T00:00:20.000Z',
          message: {
            role: 'assistant',
            content: [
              {
                type: 'text',
                text: [
                  '<remote-im-reply id="old-reply">',
                  'newer but wrong transcript reply',
                  '</remote-im-reply id="old-reply">'
                ].join('\n')
              }
            ]
          }
        })
      ].join('\n') + '\n',
      'utf8'
    )

    expect(
      readLatestClaudeRemoteImReply({
        cwd,
        projectsRoot: root,
        sinceMs: Date.parse('2026-06-29T00:00:05.000Z'),
        replyId: 'reply-current'
      })
    ).toEqual({
      content: 'current transcript reply',
      completed: true,
      replyId: 'reply-current',
      completedReplyIds: ['reply-current'],
      frameId: 'assistant-current'
    })
  })

  it('reads a matching id reply when Claude emits a legacy close tag', () => {
    const root = mkdtempSync(join(tmpdir(), 'multi-ai-code-claude-transcript-'))
    const cwd = '/Users/me/work/repo'
    const dir = getClaudeProjectTranscriptDir(cwd, root)
    mkdirSync(dir, { recursive: true })
    const transcript = join(dir, 'session.jsonl')

    writeFileSync(
      transcript,
      [
        JSON.stringify(transcriptUser('rim-current', '2026-06-29T00:00:06.000Z')),
        JSON.stringify({
          type: 'assistant',
          uuid: 'assistant-legacy-close',
          timestamp: '2026-06-29T00:00:10.000Z',
          message: {
            role: 'assistant',
            content: [
              {
                type: 'text',
                text: [
                  '<remote-im-reply id="rim-current">',
                  'current transcript reply',
                  '</remote-im-reply>'
                ].join('\n')
              }
            ]
          }
        })
      ].join('\n') + '\n',
      'utf8'
    )

    expect(
      readLatestClaudeRemoteImReply({
        cwd,
        projectsRoot: root,
        sinceMs: Date.parse('2026-06-29T00:00:05.000Z'),
        replyId: 'rim-current'
      })
    ).toEqual({
      content: 'current transcript reply',
      completed: false,
      replyId: 'rim-current',
      completedReplyIds: [],
      frameId: 'assistant-legacy-close'
    })
  })

  it('recognizes an empty exact assistant frame as completed', () => {
    const root = mkdtempSync(join(tmpdir(), 'multi-ai-code-claude-transcript-'))
    const cwd = '/Users/me/work/repo'
    const dir = getClaudeProjectTranscriptDir(cwd, root)
    mkdirSync(dir, { recursive: true })
    writeFileSync(
      join(dir, 'session.jsonl'),
      [
        JSON.stringify(transcriptUser('rim-empty', '2026-06-29T00:00:06.000Z')),
        JSON.stringify(transcriptAssistant(
          'rim-empty',
          '2026-06-29T00:00:10.000Z',
          '',
          'assistant-empty'
        ))
      ].join('\n') + '\n',
      'utf8'
    )

    expect(
      readLatestClaudeRemoteImReply({
        cwd,
        projectsRoot: root,
        sinceMs: Date.parse('2026-06-29T00:00:05.000Z'),
        replyId: 'rim-empty'
      })
    ).toEqual({
      content: '',
      completed: true,
      replyId: 'rim-empty',
      completedReplyIds: ['rim-empty'],
      frameId: 'assistant-empty'
    })
  })

  it('keeps a queued continuation pending until its own user turn materializes', () => {
    const root = mkdtempSync(join(tmpdir(), 'multi-ai-code-claude-transcript-'))
    const cwd = '/Users/me/work/repo'
    const dir = getClaudeProjectTranscriptDir(cwd, root)
    mkdirSync(dir, { recursive: true })
    const transcript = join(dir, 'session.jsonl')
    const firstTurn = [
      transcriptUser('rim-first', '2026-06-29T00:00:06.000Z', 'first'),
      {
        type: 'user',
        timestamp: '2026-06-29T00:00:08.000Z',
        message: {
          role: 'user',
          content: [
            {
              type: 'tool_result',
              content: buildRemoteImAicliPrompt({
                fromUserId: 'phone',
                text: 'terminal echo only',
                replyId: 'rim-second'
              }, { includeReplyProtocol: true })
            }
          ]
        }
      },
      transcriptAssistant(
        'rim-first',
        '2026-06-29T00:00:10.000Z',
        'first answer',
        'assistant-first'
      ),
      {
        type: 'queue-operation',
        operation: 'enqueue',
        timestamp: '2026-06-29T00:00:11.000Z',
        content: buildRemoteImAicliPrompt({
          fromUserId: 'phone',
          text: 'queued',
          replyId: 'rim-second'
        }, { includeReplyProtocol: true })
      }
    ]
    writeFileSync(transcript, firstTurn.map((entry) => JSON.stringify(entry)).join('\n') + '\n')

    expect(
      readLatestClaudeRemoteImReply({
        cwd,
        projectsRoot: root,
        sinceMs: Date.parse('2026-06-29T00:00:05.000Z'),
        pendingReplyIds: ['rim-first', 'rim-second']
      })
    ).toMatchObject({
      content: 'first answer',
      completed: true,
      replyId: 'rim-first',
      completedReplyIds: ['rim-first'],
      frameId: 'assistant-first'
    })

    const secondTurn = [
      {
        type: 'queue-operation',
        operation: 'dequeue',
        timestamp: '2026-06-29T00:00:11.500Z'
      },
      transcriptUser('rim-second', '2026-06-29T00:00:12.000Z', 'queued'),
      transcriptAssistant(
        'rim-second',
        '2026-06-29T00:00:15.000Z',
        'second answer',
        'assistant-second'
      )
    ]
    writeFileSync(
      transcript,
      [...firstTurn, ...secondTurn].map((entry) => JSON.stringify(entry)).join('\n') + '\n'
    )

    // Both assistant frames already exist, but the reader drains the earliest
    // causal barrier first so its reply cannot be skipped by a slow host poll.
    expect(
      readLatestClaudeRemoteImReply({
        cwd,
        projectsRoot: root,
        sinceMs: Date.parse('2026-06-29T00:00:05.000Z'),
        pendingReplyIds: ['rim-first', 'rim-second']
      })
    ).toMatchObject({
      content: 'first answer',
      completedReplyIds: ['rim-first'],
      frameId: 'assistant-first'
    })

    expect(
      readLatestClaudeRemoteImReply({
        cwd,
        projectsRoot: root,
        sinceMs: Date.parse('2026-06-29T00:00:05.000Z'),
        pendingReplyIds: ['rim-second']
      })
    ).toMatchObject({
      content: 'second answer',
      completed: true,
      replyId: 'rim-second',
      completedReplyIds: ['rim-second'],
      frameId: 'assistant-second'
    })
  })

  it('clears all prompts merged into one materialized user turn at one assistant frame', () => {
    const root = mkdtempSync(join(tmpdir(), 'multi-ai-code-claude-transcript-'))
    const cwd = '/Users/me/work/repo'
    const dir = getClaudeProjectTranscriptDir(cwd, root)
    mkdirSync(dir, { recursive: true })
    writeFileSync(
      join(dir, 'session.jsonl'),
      [
        {
          type: 'user',
          timestamp: '2026-06-29T00:00:06.000Z',
          message: {
            role: 'user',
            content: [
              buildRemoteImAicliPrompt({
                fromUserId: 'phone',
                text: 'first',
                replyId: 'rim-first'
              }, { includeReplyProtocol: true }),
              buildRemoteImAicliPrompt({
                fromUserId: 'phone',
                text: 'steer',
                replyId: 'rim-steer'
              }, { includeReplyProtocol: true })
            ].join('\n')
          }
        },
        transcriptAssistant(
          'rim-steer',
          '2026-06-29T00:00:10.000Z',
          'merged answer',
          'assistant-merged'
        )
      ].map((entry) => JSON.stringify(entry)).join('\n') + '\n',
      'utf8'
    )

    expect(
      readLatestClaudeRemoteImReply({
        cwd,
        projectsRoot: root,
        sinceMs: Date.parse('2026-06-29T00:00:05.000Z'),
        pendingReplyIds: ['rim-first', 'rim-steer']
      })
    ).toMatchObject({
      content: 'merged answer',
      completed: true,
      replyId: 'rim-steer',
      completedReplyIds: ['rim-first', 'rim-steer'],
      frameId: 'assistant-merged'
    })
  })
})
