import { afterEach, describe, expect, it, vi } from 'vitest'
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, symlinkSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { parseTuiHistoryRow, readTuiHistoryDiagnostics } from '../../../electron/remote-im/tuiHistoryDiagnostics.js'
const fake = vi.hoisted(() => ({ rows: [] as object[], params: [] as unknown[][], opens: [] as unknown[], closed: 0 }))
vi.mock('better-sqlite3', () => ({ default: class {
  constructor(path: string, options: unknown) { fake.opens.push({ path, options }) }
  pragma() {}
  prepare(sql: string) { return { all: (...params: unknown[]) => { fake.params.push([sql, ...params]); return fake.rows } } }
  close() { fake.closed++ }
} }))
const roots: string[] = []
afterEach(() => { roots.splice(0).forEach(root => rmSync(root, { recursive: true, force: true })); fake.rows = []; fake.params = []; fake.opens = []; fake.closed = 0 })
function root() { const path = mkdtempSync(join(tmpdir(), 'tui-diagnostics-')); roots.push(path); return path }
function row(stage = 'history_inserted') { return { ts: 100, feedback_log_body: 'tui_history ' + JSON.stringify({
  event: 'tui_history', stage, messageId: 'msg-1', cellCount: 4, replacedCells: 0, hasStream: false, text: 'PRIVATE_BODY', token: 'SECRET'
}) } }
describe('TUI history evidence', () => {
  it('keeps stage and correlation but strips bodies and unknown values', () => {
    expect(parseTuiHistoryRow(row())).toEqual({ createdAt: 100000, event: 'tui_history', stage: 'history_inserted', messageId: 'msg-1', cellCount: 4, replacedCells: 0, hasStream: false })
    expect(parseTuiHistoryRow(row('unknown'))).toBeNull()
    expect(parseTuiHistoryRow({ ts: 100, feedback_log_body: 'arbitrary log text' })).toBeNull()
  })
  it('reads only the trusted process and time range from an owned database', async () => {
    const base = root(), home = join(base, '.codex'); mkdirSync(home); writeFileSync(join(home, 'logs_2.sqlite'), '')
    fake.rows = [row()]
    const session = { cli: 'codex', codexHome: home, pid: 42, startedAt: 90500 }
    const result = await readTuiHistoryDiagnostics({ appDataRoot: base, sessions: [session, session], since: 80000, now: 110000 })
    expect(result).toMatchObject({ status: 'ok', truncated: false, invalidLines: 0, events: [parseTuiHistoryRow(row())] })
    expect(fake.params[0].slice(1)).toEqual([90, 110, 'pid:42:%'])
    expect(fake.params[0][0]).toContain("target = 'codex_tui::history_diagnostics'")
    expect(fake.opens).toHaveLength(1); expect(fake.closed).toBe(1)
    expect(fake.opens[0]).toMatchObject({ options: { readonly: true, fileMustExist: true } })
  })
  it('preserves same-timestamp sequence and millisecond boundaries', async () => {
    const base = root(), home = join(base, '.codex'); mkdirSync(home); writeFileSync(join(home, 'logs_2.sqlite'), '')
    fake.rows = [{ ...row('history_inserted'), ts_nanos: 600000000 }, { ...row('completion_received'), ts_nanos: 600000000 }]
    const result = await readTuiHistoryDiagnostics({ appDataRoot: base, sessions: [{ cli: 'codex', codexHome: home, pid: 42, startedAt: 100500 }], since: 100500, now: 100900 })
    expect(result.events.map(event => event.stage)).toEqual(['completion_received', 'history_inserted'])
    expect(result.events.map(event => event.createdAt)).toEqual([100600, 100600])
  })
  it('rejects databases symlinked outside the app root', async () => {
    const base = root(), external = root(), home = join(base, '.codex'); mkdirSync(home)
    writeFileSync(join(external, 'logs_2.sqlite'), ''); symlinkSync(join(external, 'logs_2.sqlite'), join(home, 'logs_2.sqlite'))
    expect(await readTuiHistoryDiagnostics({ appDataRoot: base, sessions: [{ cli: 'codex', codexHome: home, pid: 42, startedAt: 1 }], since: 0, now: 110000 })).toMatchObject({ status: 'unavailable', events: [] })
    expect(fake.opens).toHaveLength(0)
  })
  it('reports truncation and invalid rows explicitly', async () => {
    const base = root(), home = join(base, '.codex'); mkdirSync(home); writeFileSync(join(home, 'logs_2.sqlite'), '')
    fake.rows = Array.from({ length: 1001 }, () => row()); fake.rows[0] = row('invalid')
    expect(await readTuiHistoryDiagnostics({ appDataRoot: base, sessions: [{ cli: 'codex', codexHome: home, pid: 42, startedAt: 1 }], since: 0, now: 110000 })).toMatchObject({ status: 'partial', truncated: true, invalidLines: 1 })
  })
})
