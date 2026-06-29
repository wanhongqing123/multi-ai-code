import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs'
import { homedir } from 'node:os'
import { join } from 'node:path'
import { extractRemoteImReplyOutput } from './replyProtocol.js'

export interface ReadClaudeRemoteImReplyInput {
  cwd: string
  sinceMs: number
  projectsRoot?: string
  maxFiles?: number
}

interface ClaudeTranscriptCandidate {
  content: string
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

function getEntryTimestampMs(entry: unknown): number | null {
  if (!entry || typeof entry !== 'object') return null
  const timestamp = (entry as { timestamp?: unknown }).timestamp
  if (typeof timestamp !== 'string') return null
  const value = Date.parse(timestamp)
  return Number.isFinite(value) ? value : null
}

export function readLatestClaudeRemoteImReply(
  input: ReadClaudeRemoteImReplyInput
): string | null {
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

  const candidates: ClaudeTranscriptCandidate[] = []
  for (const { file } of files) {
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

      const timestampMs = getEntryTimestampMs(entry)
      if (timestampMs === null || timestampMs < input.sinceMs) continue

      const text = getAssistantText(entry)
      if (!text.includes('<remote-im-reply>')) continue
      const reply = extractRemoteImReplyOutput(text)
      if (reply.content.trim()) {
        candidates.push({
          content: reply.content,
          timestampMs,
          lineIndex
        })
      }
    }
  }

  candidates.sort((a, b) => b.timestampMs - a.timestampMs || b.lineIndex - a.lineIndex)
  return candidates[0]?.content ?? null
}
