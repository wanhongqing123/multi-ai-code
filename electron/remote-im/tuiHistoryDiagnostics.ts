import { promises as fs } from 'fs'
import { isAbsolute, join, relative, sep } from 'path'

const stages = new Set(['completion_received', 'history_inserted', 'history_replaced', 'empty_completion'])

/** Decode only our structured history records, never arbitrary SQLite log text. */
export function parseTuiHistoryRow(row: { ts: number; ts_nanos?: number; feedback_log_body: string }): Record<string, unknown> | null {
  const prefix = 'tui_history '
  const offset = row.feedback_log_body.indexOf(prefix)
  if (offset < 0) return null
  try {
    const value = JSON.parse(row.feedback_log_body.slice(offset + prefix.length))
    if (value.event !== 'tui_history' || !stages.has(value.stage)) return null
    const event: Record<string, unknown> = { createdAt: row.ts * 1000 + Math.floor((row.ts_nanos ?? 0) / 1000000), event: 'tui_history', stage: value.stage }
    for (const key of ['messageId', 'turnId']) {
      if (typeof value[key] === 'string' && /^[a-zA-Z0-9_.:@/-]{1,200}$/.test(value[key])) event[key] = value[key]
    }
    for (const key of ['textLength', 'visibleLength', 'cellCount', 'replacedCells']) {
      if (Number.isSafeInteger(value[key]) && value[key] >= 0) event[key] = value[key]
    }
    for (const key of ['hasStream', 'replay']) if (typeof value[key] === 'boolean') event[key] = value[key]
    return event
  } catch { return null }
}

/** Process identities come from this project's active sessions and lifecycle records. */
export async function readTuiHistoryDiagnostics(input: {
  appDataRoot: string; sessions: Array<{ cli?: string; codexHome?: string; pid?: number; startedAt?: number }>
  since: number; now: number
}) {
  const events: Record<string, unknown>[] = []
  let truncated = false, invalidLines = 0, opened = 0, unavailable = false
  const seen = new Set<string>()
  let base: string
  try { base = await fs.realpath(input.appDataRoot) } catch {
    return { source: 'codex-tui-history', status: 'unavailable', truncated, invalidLines, events }
  }
  const contains = (parent: string, child: string) => {
    const tail = relative(parent, child)
    return tail === '' || (!isAbsolute(tail) && tail !== '..' && !tail.startsWith(`..${sep}`))
  }
  for (const session of input.sessions) {
    if (session.cli !== 'codex' || !session.codexHome || !Number.isSafeInteger(session.pid) || session.pid! <= 0 ||
        !Number.isFinite(session.startedAt) || session.startedAt! > input.now) continue
    const key = `${session.codexHome}:${session.pid}:${session.startedAt}`
    if (seen.has(key)) continue
    if (seen.size >= 32) { truncated = true; break }
    seen.add(key)
    try {
      const home = await fs.realpath(session.codexHome)
      const path = await fs.realpath(join(home, 'logs_2.sqlite'))
      if (!contains(base, home) || !contains(home, path)) { unavailable = true; continue }
      const { default: Database } = await import('better-sqlite3')
      const db = new Database(path, { readonly: true, fileMustExist: true, timeout: 100 })
      try {
        db.pragma('query_only = ON')
        const rows = db.prepare(`SELECT ts, ts_nanos, feedback_log_body FROM logs
          WHERE ts >= ? AND ts <= ? AND process_uuid LIKE ?
          AND target = 'codex_tui::history_diagnostics' ORDER BY ts DESC, id DESC LIMIT 1001`)
          .all(Math.floor(Math.max(input.since, session.startedAt!) / 1000), Math.floor(input.now / 1000), `pid:${session.pid}:%`) as Array<{ ts: number; ts_nanos?: number; feedback_log_body: string }>
        opened++
        if (rows.length > 1000) truncated = true
        for (const row of rows.slice(0, 1000).reverse()) {
          const event = parseTuiHistoryRow(row)
          if (!event) invalidLines++
          else if (Number(event.createdAt) >= Math.max(input.since, session.startedAt!) && Number(event.createdAt) <= input.now) events.push(event)
        }
      } finally { db.close() }
    } catch { unavailable = true }
  }
  events.sort((a, b) => Number(a.createdAt) - Number(b.createdAt))
  if (events.length > 1000) truncated = true
  return { source: 'codex-tui-history', status: opened ? (unavailable || invalidLines ? 'partial' : events.length ? 'ok' : 'no-retained-events') : 'unavailable',
    truncated, invalidLines, events: events.slice(-1000) }
}
