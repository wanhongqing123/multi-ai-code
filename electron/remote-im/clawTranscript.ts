import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs'
import { join } from 'node:path'
import {
  buildRemoteImReplyCloseTag,
  buildRemoteImReplyOpenTag,
  extractRemoteImReplyOutput
} from './replyProtocol.js'
import type { RemoteImTranscriptReply } from './outputForwarding.js'

/**
 * claw 的回传走「读会话 JSONL」这条路，和 claude 一样，**不需要改 fork**。
 *
 * 与 claude 的三处差异：
 *
 * 1. 目录：claude 是 ~/.claude/projects/<编码后的 cwd>/，claw 是
 *    <cwd>/.claw/sessions/<workspace 指纹>/。指纹是 claw 内部算的，我们不去
 *    复刻那个哈希——直接扫 sessions 下所有子目录，用每个文件头部 session_meta
 *    里的 workspace_root 反查。复刻哈希会在它换算法时静默失配。
 *
 * 2. 记录形状：claude 是 {type:'assistant', message:{role, content:[{type:'text',text}]}}，
 *    claw 是 {type:'message', message:{role, blocks:[{type:'text',text}]}}。
 *
 * 3. **claw 的 message 记录没有时间戳**（只有 session_meta 有 created/updated_at_ms、
 *    prompt_history 有 timestamp_ms）。所以逐条按 sinceMs 过滤做不到。
 *    这不影响正确性：回传的判据本来就是 replyId——它每轮唯一，assistant 文本里
 *    带着当前 replyId 就一定是这一轮的回复。时间戳在 claude 那边也只是冗余保险。
 *    这里用文件 mtime 做粗筛，替代那层保险。
 */

export interface ReadClawRemoteImReplyInput {
  cwd: string
  sinceMs: number
  replyId?: string
  pendingReplyIds?: string[]
  maxFiles?: number
}

interface ClawTranscriptCandidate {
  content: string
  completed: boolean
  replyId?: string
  completedReplyIds: string[]
  frameId: string
  mtimeMs: number
  lineIndex: number
}

export function getClawSessionsRoot(cwd: string): string {
  return join(cwd, '.claw', 'sessions')
}

function normalizeWorkspace(value: string): string {
  return value.trim().replace(/\\/g, '/').replace(/\/+$/g, '').toLowerCase()
}

/** 从 blocks 里取纯文本；只认 type==='text'，工具块不算助手正文。 */
function getBlockText(message: unknown, role: 'assistant' | 'user'): string {
  if (!message || typeof message !== 'object') return ''
  const record = message as { role?: unknown; blocks?: unknown }
  if (record.role !== role) return ''
  if (!Array.isArray(record.blocks)) return ''
  return record.blocks
    .map((block) => {
      if (!block || typeof block !== 'object') return ''
      const item = block as { type?: unknown; text?: unknown }
      return item.type === 'text' && typeof item.text === 'string' ? item.text : ''
    })
    .filter(Boolean)
    .join('\n')
}

function entryMessage(entry: unknown): unknown {
  if (!entry || typeof entry !== 'object') return null
  const record = entry as { type?: unknown; message?: unknown }
  return record.type === 'message' ? record.message : null
}

function mentionedReplyIds(text: string): string[] {
  return [
    ...new Set(
      [...text.matchAll(/<remote-im-reply\s+id="([A-Za-z0-9_-]{1,80})/g)].map((match) => match[1])
    )
  ]
}

/** 会话文件的 workspace_root（取首行 session_meta）；读不到返回 null。 */
function readWorkspaceRoot(file: string, firstLine: string): string | null {
  try {
    const meta = JSON.parse(firstLine) as { type?: unknown; workspace_root?: unknown }
    if (meta.type !== 'session_meta') return null
    return typeof meta.workspace_root === 'string' ? meta.workspace_root : null
  } catch {
    return null
  }
}

export function readLatestClawRemoteImReply(
  input: ReadClawRemoteImReplyInput
): RemoteImTranscriptReply | null {
  const root = getClawSessionsRoot(input.cwd)
  if (!existsSync(root)) return null

  const wanted = normalizeWorkspace(input.cwd)
  const files: { file: string; mtimeMs: number }[] = []
  for (const dirEntry of readdirSync(root, { withFileTypes: true })) {
    if (!dirEntry.isDirectory()) continue
    const dir = join(root, dirEntry.name)
    for (const fileEntry of readdirSync(dir, { withFileTypes: true })) {
      if (!fileEntry.isFile() || !fileEntry.name.endsWith('.jsonl')) continue
      const file = join(dir, fileEntry.name)
      try {
        files.push({ file, mtimeMs: statSync(file).mtimeMs })
      } catch {
        /* 文件可能正在被轮转，跳过 */
      }
    }
  }

  const recent = files
    .sort((a, b) => b.mtimeMs - a.mtimeMs)
    .slice(0, input.maxFiles ?? 8)

  const pendingReplyIds = [
    ...new Set(
      (input.pendingReplyIds !== undefined
        ? input.pendingReplyIds
        : input.replyId
          ? [input.replyId]
          : []
      )
        .map((replyId) => replyId.trim())
        .filter(Boolean)
    )
  ]
  const pendingReplyIdSet = new Set(pendingReplyIds)
  const candidates: ClawTranscriptCandidate[] = []

  for (const { file, mtimeMs } of recent) {
    // 文件早于本轮开始就不可能含本轮回复。这是粗筛，真正的判据是 replyId。
    if (mtimeMs < input.sinceMs) continue

    let lines: string[]
    try {
      lines = readFileSync(file, 'utf8').split('\n')
    } catch {
      continue
    }
    const firstLine = lines.find((line) => line.trim())
    if (!firstLine) continue
    const workspaceRoot = readWorkspaceRoot(file, firstLine.trim())
    // claw 按 cwd 分目录，但同一个 sessions 根下可能有别的工作区留下的目录，
    // 必须用 session_meta 反查确认，不能只靠路径。
    if (!workspaceRoot || normalizeWorkspace(workspaceRoot) !== wanted) continue

    const materializedReplyIds = new Set<string>()
    for (let lineIndex = 0; lineIndex < lines.length; lineIndex += 1) {
      const line = lines[lineIndex]?.trim()
      if (!line) continue

      let entry: unknown
      try {
        entry = JSON.parse(line)
      } catch {
        continue
      }

      const message = entryMessage(entry)
      if (!message) continue

      const userText = getBlockText(message, 'user')
      if (userText) {
        for (const replyId of mentionedReplyIds(userText)) {
          if (
            pendingReplyIdSet.has(replyId) &&
            userText.includes(buildRemoteImReplyOpenTag(replyId)) &&
            userText.includes(buildRemoteImReplyCloseTag(replyId))
          ) {
            materializedReplyIds.add(replyId)
          }
        }
        continue
      }

      const text = getBlockText(message, 'assistant')
      if (!text) continue

      const candidateReplyIds = mentionedReplyIds(text).filter((replyId) =>
        pendingReplyIdSet.has(replyId)
      )
      const replies =
        pendingReplyIds.length > 0
          ? candidateReplyIds.map((replyId) => ({
              replyId,
              extraction: extractRemoteImReplyOutput(text, { replyId })
            }))
          : [{ replyId: undefined, extraction: extractRemoteImReplyOutput(text) }]
      const exact = [...replies].reverse().find(({ extraction }) => extraction.completed)
      const compatible = [...replies]
        .reverse()
        .find(({ extraction }) => extraction.content.trim() || extraction.pending)
      const selected = exact ?? compatible
      if (selected && (selected.extraction.content.trim() || selected.extraction.completed)) {
        candidates.push({
          content: selected.extraction.content,
          completed: selected.extraction.completed,
          ...(selected.replyId ? { replyId: selected.replyId } : {}),
          completedReplyIds: selected.extraction.completed
            ? pendingReplyIds.filter((replyId) => materializedReplyIds.has(replyId))
            : [],
          frameId: `${file}:${lineIndex}`,
          mtimeMs,
          lineIndex
        })
      }
    }
  }

  // 与 claude 侧同一条排序规则：优先放行「已完成且 prompt 已落地」的那一帧，
  // 一次只推进一个因果屏障——两轮都在轮询间隙完成时，只回最新帧会吞掉前一轮的回复。
  const advancing = candidates
    .filter((candidate) => candidate.completed && candidate.completedReplyIds.length > 0)
    .sort((a, b) => a.mtimeMs - b.mtimeMs || a.lineIndex - b.lineIndex)
  candidates.sort((a, b) => b.mtimeMs - a.mtimeMs || b.lineIndex - a.lineIndex)

  const latest = advancing[0] ?? candidates[0]
  return latest
    ? {
        content: latest.content,
        completed: latest.completed,
        ...(latest.replyId ? { replyId: latest.replyId } : {}),
        completedReplyIds: latest.completedReplyIds,
        frameId: latest.frameId
      }
    : null
}
