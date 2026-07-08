import { existsSync, readdirSync, statSync } from 'node:fs'
import { homedir } from 'node:os'
import { join, isAbsolute } from 'node:path'
import Database from 'better-sqlite3'
import { extractRemoteImReplyOutput } from './replyProtocol.js'

export interface ReadOpenCodeRemoteImReplyInput {
  cwd: string
  sinceMs: number
  replyId?: string
  dataDir?: string
  dbPaths?: string[]
  maxDbs?: number
  lookbackMs?: number
}

interface OpenCodePartRow {
  session_id: string
  time_created: number
  data: string
}

interface OpenCodeReplyCandidate {
  content: string
  timestampMs: number
}

function defaultOpenCodeDataDirs(): string[] {
  const dirs = [
    join(process.env.XDG_DATA_HOME || join(homedir(), '.local', 'share'), 'opencode')
  ]
  if (process.platform === 'win32') {
    const localAppData = process.env.LOCALAPPDATA
    if (localAppData) dirs.push(join(localAppData, 'opencode'))
  }
  return [...new Set(dirs)]
}

function defaultOpenCodeDbPath(dataDir: string): string | null {
  const configured = process.env.OPENCODE_DB?.trim()
  if (!configured) return null
  if (configured === ':memory:') return null
  return isAbsolute(configured) ? configured : join(dataDir, configured)
}

export function listOpenCodeDbCandidates(input: {
  dataDir?: string
  dbPaths?: string[]
  maxDbs?: number
} = {}): string[] {
  const explicit = input.dbPaths?.filter(Boolean) ?? []
  const dirs = input.dataDir ? [input.dataDir] : defaultOpenCodeDataDirs()
  const discovered: Array<{ file: string; mtimeMs: number }> = []

  for (const dir of dirs) {
    const configured = defaultOpenCodeDbPath(dir)
    if (configured && existsSync(configured)) {
      discovered.push({ file: configured, mtimeMs: statSync(configured).mtimeMs })
    }

    if (!existsSync(dir)) continue
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      if (!entry.isFile()) continue
      if (!/^opencode(?:-[A-Za-z0-9._-]+)?\.db$/.test(entry.name)) continue
      const file = join(dir, entry.name)
      discovered.push({ file, mtimeMs: statSync(file).mtimeMs })
    }
  }

  const all = [
    ...explicit.map((file) => ({ file, mtimeMs: existsSync(file) ? statSync(file).mtimeMs : 0 })),
    ...discovered
  ]
  return [...new Map(all.map((item) => [item.file, item])).values()]
    .filter((item) => existsSync(item.file))
    .sort((a, b) => b.mtimeMs - a.mtimeMs)
    .slice(0, input.maxDbs ?? 6)
    .map((item) => item.file)
}

function readTextPart(data: string): string {
  try {
    const parsed = JSON.parse(data) as { type?: unknown; text?: unknown }
    return parsed.type === 'text' && typeof parsed.text === 'string' ? parsed.text : ''
  } catch {
    return ''
  }
}

function queryReplyCandidates(input: {
  dbPath: string
  cwd: string
  replyId?: string
  sinceMs: number
  lookbackMs: number
  requireCwd: boolean
}): OpenCodeReplyCandidate[] {
  const db = new Database(input.dbPath, { readonly: true, fileMustExist: true })
  try {
    const minTime = Math.max(0, input.sinceMs - input.lookbackMs)
    const pattern = input.replyId ? `%${input.replyId}%` : '%<remote-im-reply%'
    const rows = db
      .prepare(
        [
          'SELECT p.session_id, p.time_created, p.data',
          'FROM part p',
          'JOIN session s ON s.id = p.session_id',
          'WHERE p.time_created >= ?',
          'AND p.data LIKE ?',
          input.requireCwd ? 'AND s.directory = ?' : '',
          'ORDER BY p.time_created DESC',
          'LIMIT 64'
        ]
          .filter(Boolean)
          .join(' ')
      )
      .all(...(input.requireCwd ? [minTime, pattern, input.cwd] : [minTime, pattern])) as OpenCodePartRow[]

    return rows.flatMap((row) => {
      const text = readTextPart(row.data)
      if (!text) return []
      const reply = extractRemoteImReplyOutput(text, { replyId: input.replyId })
      if (!reply.content.trim()) return []
      return [{ content: reply.content, timestampMs: row.time_created }]
    })
  } catch {
    return []
  } finally {
    db.close()
  }
}

export function readLatestOpenCodeRemoteImReply(
  input: ReadOpenCodeRemoteImReplyInput
): string | null {
  const dbPaths = listOpenCodeDbCandidates({
    dataDir: input.dataDir,
    dbPaths: input.dbPaths,
    maxDbs: input.maxDbs
  })
  const lookbackMs = input.lookbackMs ?? 10 * 60 * 1000
  const candidates: OpenCodeReplyCandidate[] = []

  for (const dbPath of dbPaths) {
    candidates.push(
      ...queryReplyCandidates({
        dbPath,
        cwd: input.cwd,
        replyId: input.replyId,
        sinceMs: input.sinceMs,
        lookbackMs,
        requireCwd: true
      })
    )
  }

  if (candidates.length === 0 && input.replyId) {
    for (const dbPath of dbPaths) {
      candidates.push(
        ...queryReplyCandidates({
          dbPath,
          cwd: input.cwd,
          replyId: input.replyId,
          sinceMs: input.sinceMs,
          lookbackMs,
          requireCwd: false
        })
      )
    }
  }

  candidates.sort((a, b) => b.timestampMs - a.timestampMs)
  return candidates[0]?.content ?? null
}
