import { mkdtempSync, mkdirSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import {
  getClaudeProjectTranscriptDir,
  readLatestClaudeRemoteImReply
} from './claudeTranscript.js'

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
    ).toBe(
      ['## 目录结构', '| 目录 | 作用 |', '|------|------|', '| `chrome/` | 浏览器主体 |', '| `content/` | 渲染引擎 |'].join(
        '\n'
      )
    )
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
        JSON.stringify({
          type: 'assistant',
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
    ).toBe('current transcript reply')
  })
})
