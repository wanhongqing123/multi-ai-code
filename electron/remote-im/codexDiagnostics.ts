import { promises as fs } from 'fs'
import { homedir } from 'os'
import { isAbsolute, join, relative, resolve, sep } from 'path'

function pathKey(value: string): string {
  const path = resolve(value)
  return process.platform === 'win32' || process.platform === 'darwin' ? path.toLowerCase() : path
}

function contains(parent: string, candidate: string): boolean {
  const tail = relative(pathKey(parent), pathKey(candidate))
  return tail === '' || (!isAbsolute(tail) && tail !== '..' && !tail.startsWith(`..${sep}`))
}

/** Read only metadata from matching project rollouts in app-owned homes. */
export async function readCodexDiagnosticEvents(input: {
  appDataRoot: string; targetRepo: string; homes: string[]; since: number; now: number
}) {
  const events: object[] = []
  let truncated = false
  let invalidLines = 0
  let inspected = 0
  let matched = 0
  let inaccessible = false
  let folders = 0
  if (new Set(input.homes).size > 4) truncated = true
  let base: string
  try { base = await fs.realpath(input.appDataRoot) } catch {
    return { source: 'codex-original-events', status: 'unavailable', truncated, invalidLines, events }
  }
  const forbidden = await fs.realpath(join(homedir(), '.codex')).catch(() => join(homedir(), '.codex'))
  const candidates: Array<{ path: string; mtime: number }> = []
  async function walk(dir: string, depth: number): Promise<void> {
    if (folders++ >= 64 || candidates.length >= 128) { truncated = true; return }
    const entries = await fs.readdir(dir, { withFileTypes: true }).catch(() => [])
    for (const entry of entries.sort((a, b) => b.name.localeCompare(a.name))) {
      if (entry.isSymbolicLink()) continue
      if (entry.isDirectory() && depth < 3 && /^\d{2,4}$/.test(entry.name)) {
        await walk(join(dir, entry.name), depth + 1)
      } else if (entry.isFile() && /^rollout-.*\.jsonl$/.test(entry.name)) {
        const file = join(dir, entry.name)
        const stat = await fs.stat(file)
        if (stat.mtimeMs >= input.since) candidates.push({ path: file, mtime: stat.mtimeMs })
      }
      if (candidates.length >= 128) { truncated = true; break }
    }
  }
  for (const home of [...new Set(input.homes)].slice(0, 4)) {
    try {
      const canonical = await fs.realpath(home)
      if (!contains(base, canonical) || contains(forbidden, canonical)) { inaccessible = true; continue }
      const sessionRoot = join(canonical, 'sessions')
      const stat = await fs.lstat(sessionRoot)
      if (!stat.isDirectory() || stat.isSymbolicLink()) { inaccessible = true; continue }
      await walk(sessionRoot, 0)
    } catch { inaccessible = true }
  }
  candidates.sort((a, b) => b.mtime - a.mtime)
  if (candidates.length > 24) truncated = true
  for (const candidate of candidates.slice(0, 24)) {
    try {
      const file = await fs.open(candidate.path, 'r')
      try {
        inspected++
        const size = (await file.stat()).size
        const header = Buffer.alloc(Math.min(size, 64 * 1024))
        const first = await file.read(header, 0, header.length, 0)
        const meta = JSON.parse(header.subarray(0, first.bytesRead).toString('utf8').split('\n')[0])
        if (meta.type !== 'session_meta' || typeof meta.payload?.cwd !== 'string' ||
            pathKey(meta.payload.cwd) !== pathKey(input.targetRepo)) continue
        matched++
        const start = Math.max(0, size - 2 * 1024 * 1024)
        const buffer = Buffer.alloc(Math.min(size, 2 * 1024 * 1024))
        const tail = await file.read(buffer, 0, buffer.length, start)
        let text = buffer.subarray(0, tail.bytesRead).toString('utf8')
        if (start > 0) { text = text.slice(text.indexOf('\n') + 1); truncated = true }
        for (const line of text.split('\n').filter(Boolean)) {
          try {
            const row = JSON.parse(line)
            const at = Date.parse(row.timestamp)
            if (!Number.isFinite(at) || at < input.since || at > input.now) continue
            const payload = row.payload
            if (row.type === 'event_msg' && payload?.type === 'item_completed' && payload.item?.type === 'AgentMessage') {
              events.push({ createdAt: at, event: 'item_completed', threadId: payload.thread_id ?? meta.payload.id,
                turnId: payload.turn_id, messageId: payload.item.id, phase: payload.item.phase,
                delivery: payload.item.delivery, kind: 'AgentMessage' })
            } else if (row.type === 'response_item' && payload?.type === 'function_call' && payload.name === 'request_user_input_async') {
              events.push({ createdAt: at, event: 'function_call', threadId: meta.payload.id,
                messageId: payload.call_id, kind: 'request_user_input_async' })
            }
          } catch { invalidLines++ }
        }
      } finally { await file.close() }
    } catch { invalidLines++ }
  }
  events.sort((a, b) => (a as { createdAt: number }).createdAt - (b as { createdAt: number }).createdAt)
  if (events.length > 1000) truncated = true
  return { source: 'codex-original-events', status: matched ? (invalidLines || inaccessible ? 'partial' : 'ok') : inspected ? 'no-matching-project' : 'unavailable',
    truncated, invalidLines, events: events.slice(-1000) }
}
