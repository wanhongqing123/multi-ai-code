import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs'
import { homedir } from 'node:os'
import { basename, join } from 'node:path'
import {
  buildRemoteImReplyCloseTag,
  buildRemoteImReplyOpenTag,
  extractRemoteImReplyOutput
} from './replyProtocol.js'
import type { RemoteImTranscriptReply } from './outputForwarding.js'

export interface ReadClaudeRemoteImReplyInput {
  cwd: string
  sinceMs: number
  replyId?: string
  pendingReplyIds?: string[]
  projectsRoot?: string
  maxFiles?: number
}

interface ClaudeTranscriptCandidate {
  content: string
  completed: boolean
  replyId?: string
  completedReplyIds: string[]
  frameId?: string
  timestampMs: number
  lineIndex: number
}

function getDefaultClaudeProjectsRoot(): string {
  return join(homedir(), '.claude', 'projects')
}

function encodeClaudeProjectPath(cwd: string): string {
  const normalized = cwd.trim().replace(/\\/g, '/').replace(/\/+$/g, '')
  return (normalized || '/').replace(/\//g, '-').replace(/:/g, '')
}

export function getClaudeProjectTranscriptDir(
  cwd: string,
  projectsRoot = getDefaultClaudeProjectsRoot()
): string {
  return join(projectsRoot, encodeClaudeProjectPath(cwd))
}

function getAssistantText(entry: unknown): string {
  if (!entry || typeof entry !== 'object') return ''
  const record = entry as {
    type?: unknown
    message?: {
      role?: unknown
      content?: unknown
    }
  }
  if (record.type !== 'assistant' || record.message?.role !== 'assistant') return ''
  if (!Array.isArray(record.message.content)) return ''
  return record.message.content
    .map((part) => {
      if (!part || typeof part !== 'object') return ''
      const item = part as { type?: unknown; text?: unknown }
      return item.type === 'text' && typeof item.text === 'string' ? item.text : ''
    })
    .filter(Boolean)
    .join('\n')
}

function getUserText(entry: unknown): string {
  if (!entry || typeof entry !== 'object') return ''
  const record = entry as {
    type?: unknown
    message?: {
      role?: unknown
      content?: unknown
    }
  }
  if (record.type !== 'user' || record.message?.role !== 'user') return ''
  const content = record.message.content
  if (typeof content === 'string') return content
  if (!Array.isArray(content)) return ''
  return content
    .map((part) => {
      if (!part || typeof part !== 'object') return ''
      const item = part as { type?: unknown; text?: unknown }
      if (item.type === 'text' && typeof item.text === 'string') return item.text
      // Tool results also use role=user in Anthropic transcripts. Their
      // `content` may echo terminal text, but it is not a materialized human
      // prompt and therefore must not advance the causal watermark.
      return ''
    })
    .filter(Boolean)
    .join('\n')
}

function getEntryFrameId(entry: unknown, file: string, lineIndex: number): string {
  if (!entry || typeof entry !== 'object') return `${basename(file)}:${lineIndex}`
  const record = entry as { uuid?: unknown; message?: { id?: unknown } }
  if (typeof record.uuid === 'string' && record.uuid.trim()) return record.uuid.trim()
  return typeof record.message?.id === 'string' && record.message.id.trim()
    ? record.message.id.trim()
    : `${basename(file)}:${lineIndex}`
}

function mentionedReplyIds(text: string): string[] {
  return [
    ...new Set(
      [...text.matchAll(/<remote-im-reply\s+id="([A-Za-z0-9_-]{1,80})/g)].map(
        (match) => match[1]
      )
    )
  ]
}

function getEntryTimestampMs(entry: unknown): number | null {
  if (!entry || typeof entry !== 'object') return null
  const timestamp = (entry as { timestamp?: unknown }).timestamp
  if (typeof timestamp !== 'string') return null
  const value = Date.parse(timestamp)
  return Number.isFinite(value) ? value : null
}

export function readLatestClaudeRemoteImReply(
  input: ReadClaudeRemoteImReplyInput
): RemoteImTranscriptReply | null {
  const dir = getClaudeProjectTranscriptDir(input.cwd, input.projectsRoot)
  if (!existsSync(dir)) return null

  const files = readdirSync(dir, { withFileTypes: true })
    .filter((entry) => entry.isFile() && entry.name.endsWith('.jsonl'))
    .map((entry) => {
      const file = join(dir, entry.name)
      return { file, mtimeMs: statSync(file).mtimeMs }
    })
    .sort((a, b) => b.mtimeMs - a.mtimeMs)
    .slice(0, input.maxFiles ?? 8)

  const pendingReplyIds = [
    ...new Set(
      (input.pendingReplyIds !== undefined
        ? input.pendingReplyIds
        : input.replyId
          ? [input.replyId]
          : []
      ).map((replyId) => replyId.trim()).filter(Boolean)
    )
  ]
  const pendingReplyIdSet = new Set(pendingReplyIds)
  const candidates: ClaudeTranscriptCandidate[] = []
  for (const { file } of files) {
    const materializedReplyIds = new Set<string>()
    const lines = readFileSync(file, 'utf8').split('\n')
    for (let lineIndex = 0; lineIndex < lines.length; lineIndex += 1) {
      const line = lines[lineIndex]?.trim()
      if (!line) continue

      let entry: unknown
      try {
        entry = JSON.parse(line)
      } catch {
        continue
      }

      const userText = getUserText(entry)
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
      }

      const timestampMs = getEntryTimestampMs(entry)
      if (timestampMs === null || timestampMs < input.sinceMs) continue

      const text = getAssistantText(entry)
      if (!text) continue
      const candidateReplyIds = mentionedReplyIds(text).filter((replyId) =>
        pendingReplyIdSet.has(replyId)
      )
      const replies = pendingReplyIds.length > 0
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
        const frameId = getEntryFrameId(entry, file, lineIndex)
        candidates.push({
          content: selected.extraction.content,
          completed: selected.extraction.completed,
          ...(selected.replyId ? { replyId: selected.replyId } : {}),
          completedReplyIds: selected.extraction.completed
            ? pendingReplyIds.filter((replyId) => materializedReplyIds.has(replyId))
            : [],
          frameId,
          timestampMs,
          lineIndex
        })
      }
    }
  }

  const advancing = candidates
    .filter((candidate) => candidate.completed && candidate.completedReplyIds.length > 0)
    .sort((a, b) => a.timestampMs - b.timestampMs || a.lineIndex - b.lineIndex)
  candidates.sort((a, b) => b.timestampMs - a.timestampMs || b.lineIndex - a.lineIndex)
  // Drain one causal barrier at a time. If two queued turns both finish before
  // the host polls, returning only the newest frame would consume both prompt
  // ids while silently dropping the first assistant reply.
  const latest = advancing[0] ?? candidates[0]
  return latest
    ? {
        content: latest.content,
        completed: latest.completed,
        ...(latest.replyId ? { replyId: latest.replyId } : {}),
        completedReplyIds: latest.completedReplyIds,
        ...(latest.frameId ? { frameId: latest.frameId } : {})
      }
    : null
}
