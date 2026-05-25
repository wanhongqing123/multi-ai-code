import { execFile } from 'child_process'
import { promises as fs } from 'fs'
import { join } from 'path'
import { listHabitEventsSince, type HabitEventRow } from '../habit/db.js'

export interface GitCommit {
  hash: string
  subject: string
  /** Unix epoch seconds. */
  ts: number
  author: string
}

/**
 * Reads the last N commits from `git log` (no body / no patch — subject only).
 * Returns an empty array on any failure (non-repo dir, no git on PATH, etc.)
 * so the summarizer can still run from whatever other sources are available.
 */
export function gitRecentCommits(
  repoPath: string,
  limit = 50,
  opts: { timeoutMs?: number } = {}
): Promise<GitCommit[]> {
  return new Promise((resolve) => {
    const timeoutMs = opts.timeoutMs ?? 8000
    const child = execFile(
      'git',
      [
        '-C',
        repoPath,
        'log',
        `-${Math.max(1, Math.floor(limit))}`,
        '--pretty=format:%h|%at|%an|%s',
        '--no-color'
      ],
      { timeout: timeoutMs, maxBuffer: 1_000_000, windowsHide: true },
      (err, stdout) => {
        if (err) {
          resolve([])
          return
        }
        resolve(parseGitLog(stdout))
      }
    )
    child.on('error', () => resolve([]))
  })
}

/** Pure parser separated out for testability. */
export function parseGitLog(stdout: string): GitCommit[] {
  if (!stdout) return []
  const out: GitCommit[] = []
  for (const line of stdout.split(/\r?\n/)) {
    if (!line) continue
    const parts = line.split('|')
    if (parts.length < 4) continue
    const [hashRaw, atStr, authorRaw, ...subjectParts] = parts
    const hash = hashRaw.trim()
    if (!hash) continue
    // Reject blank/zero timestamps too — they signal a malformed pipe row
    // (e.g., "|||") rather than a legitimate epoch.
    if (!atStr || atStr.trim() === '') continue
    const ts = Number(atStr)
    if (!Number.isFinite(ts) || ts <= 0) continue
    const subject = subjectParts.join('|').trim()
    if (!subject) continue
    out.push({
      hash,
      subject,
      ts,
      author: authorRaw.trim()
    })
  }
  return out
}

export interface RecentFile {
  /** Repo-relative posix-style path. */
  relPath: string
  /** Last modification time, unix epoch ms. */
  mtimeMs: number
}

/**
 * Walks `src` (and a small set of common source dirs) recursively for files
 * modified within the last `withinDays` days. Returns up to `limit`
 * paths sorted by mtime descending. Bounded by a hard file count cap to
 * keep huge monorepos from stalling the scheduler tick.
 */
export async function recentlyChangedFiles(
  repoPath: string,
  withinDays = 30,
  limit = 30
): Promise<RecentFile[]> {
  const cutoff = Date.now() - withinDays * 24 * 60 * 60 * 1000
  const found: RecentFile[] = []
  const dayMs = 24 * 60 * 60 * 1000
  // Heuristic source roots — we don't want to scan node_modules / dist / etc.
  const SOURCE_ROOTS = ['src', 'electron', 'lib', 'app', 'pkg', 'cmd', 'packages']
  const SKIP_DIRS = new Set([
    'node_modules',
    '.git',
    'dist',
    'build',
    'out',
    'release',
    '.next',
    '.turbo',
    '.cache',
    'coverage',
    '__pycache__',
    'target',
    'vendor'
  ])
  const HARD_FILE_CAP = 5000
  let visited = 0

  async function walk(absDir: string, relDir: string): Promise<void> {
    if (visited >= HARD_FILE_CAP) return
    let entries: import('fs').Dirent[]
    try {
      entries = await fs.readdir(absDir, { withFileTypes: true })
    } catch {
      return
    }
    for (const ent of entries) {
      if (visited >= HARD_FILE_CAP) return
      if (ent.name.startsWith('.')) continue
      const absPath = join(absDir, ent.name)
      const rel = relDir ? `${relDir}/${ent.name}` : ent.name
      if (ent.isDirectory()) {
        if (SKIP_DIRS.has(ent.name)) continue
        await walk(absPath, rel)
        continue
      }
      if (!ent.isFile()) continue
      visited++
      try {
        const st = await fs.stat(absPath)
        if (st.mtimeMs >= cutoff) {
          found.push({ relPath: rel, mtimeMs: st.mtimeMs })
        }
      } catch {
        /* file may have been deleted between readdir and stat — ignore */
      }
    }
  }

  // Walk each known source root if it exists; if none exist, walk the repo
  // root with the same skip set as a final fallback.
  let scanned = false
  for (const root of SOURCE_ROOTS) {
    const abs = join(repoPath, root)
    try {
      const st = await fs.stat(abs)
      if (st.isDirectory()) {
        scanned = true
        await walk(abs, root)
      }
    } catch {
      /* root not present, skip */
    }
  }
  if (!scanned) {
    await walk(repoPath, '')
  }

  found.sort((a, b) => b.mtimeMs - a.mtimeMs)
  // Older than `withinDays` already filtered above; clamp `limit` last.
  return found.slice(0, Math.max(1, Math.floor(limit)))
}

/**
 * Returns user-prompt habit events emitted after `sinceTs`, filtered to the
 * given repo (or unfiltered when `repoPath` is null — useful for the test
 * harness). Reads via the habit DB to avoid duplicating storage.
 */
export function userPromptsSince(
  sinceTs: number,
  repoPath: string | null
): HabitEventRow[] {
  let rows
  try {
    rows = listHabitEventsSince(Math.max(0, sinceTs))
  } catch {
    return []
  }
  const keep = rows.filter(
    (r) => r.kind === 'ai_prompt_main' || r.kind === 'ai_prompt_repo'
  )
  if (!repoPath) return keep
  return keep.filter((r) => !r.repo_path || r.repo_path === repoPath)
}

/**
 * Extracts the plain user text out of a habit event payload. The payload is
 * a JSON blob with shape `{text, ...extras}` written by the collector.
 */
export function extractHabitText(row: HabitEventRow): string {
  try {
    const parsed = JSON.parse(row.payload) as { text?: string }
    return typeof parsed.text === 'string' ? parsed.text : ''
  } catch {
    return ''
  }
}
