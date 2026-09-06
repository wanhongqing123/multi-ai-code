import { describe, expect, it } from 'vitest'
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import {
  getClawSessionsRoot,
  readLatestClawRemoteImReply
} from '../../../electron/remote-im/clawTranscript.js'

// 这些记录形状是从 claw 真实写出的 session JSONL 抄来的，不是手工设计的：
//   {"type":"session_meta","workspace_root":"...","session_id":"...","version":1,...}
//   {"type":"prompt_history","text":"...","timestamp_ms":...}
//   {"type":"message","message":{"role":"user","blocks":[{"type":"text","text":"..."}]}}
//   {"type":"message","message":{"role":"assistant","blocks":[...],"usage":{...}}}
// 关键差异（相对 claude）：用 blocks 不是 content，且 message 记录**没有时间戳**。
function sessionMeta(workspaceRoot: string): string {
  return JSON.stringify({
    type: 'session_meta',
    version: 1,
    session_id: 'session-1-0',
    model: 'openai/glm-4.6',
    workspace_root: workspaceRoot,
    created_at_ms: 1_788_707_729_009,
    updated_at_ms: 1_788_707_738_464
  })
}

function message(role: 'user' | 'assistant', text: string): string {
  return JSON.stringify({
    type: 'message',
    message: {
      role,
      blocks: [{ type: 'text', text }],
      ...(role === 'assistant' ? { usage: { input_tokens: 0, output_tokens: 0 } } : {})
    }
  })
}

function writeSession(cwd: string, dirName: string, lines: string[]): string {
  const dir = join(getClawSessionsRoot(cwd), dirName)
  mkdirSync(dir, { recursive: true })
  const file = join(dir, 'session-1-0.jsonl')
  writeFileSync(file, lines.join('\n') + '\n', 'utf8')
  return file
}

function withWorkspace(run: (cwd: string) => void): void {
  const cwd = mkdtempSync(join(tmpdir(), 'claw-tr-'))
  try {
    run(cwd)
  } finally {
    rmSync(cwd, { recursive: true, force: true })
  }
}

const ID = 'rim-abc123'
const OPEN = `<remote-im-reply id="${ID}">`
const CLOSE = `</remote-im-reply id="${ID}">`

describe('readLatestClawRemoteImReply', () => {
  it('extracts a completed reply written in claw session JSONL shape', () => {
    withWorkspace((cwd) => {
      writeSession(cwd, 'ee20245433f4020b', [
        sessionMeta(cwd),
        message('user', `请介绍一下\n${OPEN}\n(占位)\n${CLOSE}`),
        message('assistant', `思考过程不该外发。\n${OPEN}\n这是要回传的正文。\n${CLOSE}`)
      ])

      const reply = readLatestClawRemoteImReply({ cwd, sinceMs: 0, replyId: ID })
      expect(reply).not.toBeNull()
      expect(reply?.completed).toBe(true)
      expect(reply?.replyId).toBe(ID)
      expect(reply?.content.trim()).toBe('这是要回传的正文。')
      // marker 外的思考过程不能混进回传正文。
      expect(reply?.content).not.toContain('思考过程')
      // prompt 已落地 → 允许推进因果屏障。
      expect(reply?.completedReplyIds).toContain(ID)
    })
  })

  // claw 按 cwd 建 .claw/，但同一个 sessions 根下可能残留别的工作区的目录
  // （换目录跑过、或手工拷贝过）。只认路径不看 session_meta 就会回传错会话的内容。
  it('ignores session files whose session_meta points at another workspace', () => {
    withWorkspace((cwd) => {
      writeSession(cwd, 'other-workspace-hash', [
        sessionMeta('C:/some/other/repo'),
        message('user', `${OPEN}\n(占位)\n${CLOSE}`),
        message('assistant', `${OPEN}\n别的工作区的内容\n${CLOSE}`)
      ])

      expect(readLatestClawRemoteImReply({ cwd, sinceMs: 0, replyId: ID })).toBeNull()
    })
  })

  it('does not return a reply for an id that is not pending', () => {
    withWorkspace((cwd) => {
      writeSession(cwd, 'hash', [
        sessionMeta(cwd),
        message('user', `${OPEN}\n(占位)\n${CLOSE}`),
        message('assistant', `${OPEN}\n正文\n${CLOSE}`)
      ])

      const reply = readLatestClawRemoteImReply({
        cwd,
        sinceMs: 0,
        replyId: 'rim-someotherid'
      })
      expect(reply).toBeNull()
    })
  })

  // 只有 assistant 帧、没有落地的 user prompt 时，不能推进因果屏障——
  // 否则一个未真正送达模型的请求会被当成已完成。
  it('withholds the causal barrier when the prompt never materialized', () => {
    withWorkspace((cwd) => {
      writeSession(cwd, 'hash', [
        sessionMeta(cwd),
        message('assistant', `${OPEN}\n正文\n${CLOSE}`)
      ])

      const reply = readLatestClawRemoteImReply({ cwd, sinceMs: 0, replyId: ID })
      expect(reply?.content.trim()).toBe('正文')
      expect(reply?.completedReplyIds).toEqual([])
    })
  })

  it('returns null when no claw session directory exists', () => {
    withWorkspace((cwd) => {
      expect(readLatestClawRemoteImReply({ cwd, sinceMs: 0, replyId: ID })).toBeNull()
    })
  })
})
