import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { afterEach, describe, expect, it } from 'vitest'
import { SessionDiagnostics, buildSessionDiagnosticsReport } from '../../../electron/cc/sessionDiagnostics.js'

const roots: string[] = []
function tempRoot(): string {
  const root = mkdtempSync(join(tmpdir(), 'aicli-diagnostics-test-'))
  roots.push(root)
  return root
}
afterEach(() => { for (const root of roots.splice(0)) rmSync(root, { recursive: true, force: true }) })
function events(root: string): any[] {
  return readFileSync(join(root, 'logs', 'aicli-lifecycle.jsonl'), 'utf8').trim().split('\n').map(line => JSON.parse(line))
}
const context = { sessionId: 'session-1', projectId: 'project-1', cli: 'codex', appVersion: '0.1.73' }

describe('AICLI lifecycle diagnostics', () => {
  it('persists the host stop request before the actual exit, with PID and control context', () => {
    const root = tempRoot()
    let now = 1000
    const recorder = new SessionDiagnostics(root, context, () => now)
    recorder.attach(() => ({ pid: 34148, executable: '/bin/codex', lastInputAt: 1050, lastOutputAt: 1080, lastEtxAt: null }))
    recorder.spawned()
    now = 1100
    recorder.control('model')
    now = 1300
    recorder.stop('project-target-changed', 'project-2')
    // This assertion runs before exited(), as the forensic record must survive kill.
    expect(events(root).at(-1)).toMatchObject({ event: 'host-stop-requested', hostStopReason: 'project-target-changed', requestedProjectId: 'project-2' })
    now = 1600
    recorder.exited(-1073741510)
    expect(events(root).at(-1)).toMatchObject({ event: 'process-exit', pid: 34148, exitCode: -1073741510, exitCodeHex: '0xc000013a', signal: null, lifetimeMs: 600, sinceInputMs: 550, lastControl: 'model', hostStopReason: 'project-target-changed' })
  })

  it('does not label an unobserved stop as a host kill', () => {
    const root = tempRoot()
    const recorder = new SessionDiagnostics(root, context)
    recorder.exited(-1073741510)
    expect(events(root).at(-1)).toMatchObject({ hostStopReason: null, stopRequestedAt: null, pid: null })
  })

  it('exports startup error codes without including error messages or untrusted codes', () => {
    const root = tempRoot()
    const recorder = new SessionDiagnostics(root, context)
    recorder.spawnFailed(Object.assign(new Error('private command and secret-token'), { code: 'ENOENT' }))
    recorder.spawnFailed({ code: 'secret-token' })
    const report = buildSessionDiagnosticsReport(root, 'test') as any
    const failures = report.files.flatMap((file: any) => file.events).filter((event: any) => event.event === 'spawn-failed')
    expect(failures).toHaveLength(2)
    expect(failures[0].spawnErrorCode).toBe('ENOENT')
    expect(failures[1].spawnErrorCode).toBeNull()
    expect(JSON.stringify(report)).not.toContain('private command')
    expect(JSON.stringify(report)).not.toContain('secret-token')
  })

  it('exports each stop request reason while retaining the first stop context', () => {
    const root = tempRoot()
    let now = 100
    const recorder = new SessionDiagnostics(root, context, () => now)
    recorder.stop('cc:kill')
    now = 200
    recorder.stop('before-quit')
    recorder.exited(-1073741510)
    const report = buildSessionDiagnosticsReport(root, 'test') as any
    const recorded = report.files.flatMap((file: any) => file.events)
    const stops = recorded.filter((event: any) => event.event === 'host-stop-requested')
    expect(stops).toHaveLength(2)
    expect(stops[0]).toMatchObject({ stopReasonRequested: 'cc:kill', hostStopReason: 'cc:kill', stopRequestedAt: 100, createdAt: 100 })
    expect(stops[1]).toMatchObject({ stopReasonRequested: 'before-quit', hostStopReason: 'cc:kill', stopRequestedAt: 100, createdAt: 200 })
    expect(recorded.at(-1)).toMatchObject({ event: 'process-exit', hostStopReason: 'cc:kill', stopRequestedAt: 100 })
  })

  it('isolates persistence failures by account root', () => {
    const parent = tempRoot()
    const failedRoot = join(parent, 'failed-account')
    writeFileSync(failedRoot, 'not a directory')
    new SessionDiagnostics(failedRoot, context)
    const healthyRoot = tempRoot()
    new SessionDiagnostics(healthyRoot, context)
    expect((buildSessionDiagnosticsReport(failedRoot, 'test') as any).persistenceError).toBeTruthy()
    expect((buildSessionDiagnosticsReport(healthyRoot, 'test') as any).persistenceError).toBeNull()
    // Another account's successful write must not erase the failed account's warning.
    expect((buildSessionDiagnosticsReport(failedRoot, 'test') as any).persistenceError).toBeTruthy()
  })

  it('exports the real persisted exit after no active sessions remain', () => {
    const root = tempRoot()
    const recorder = new SessionDiagnostics(root, context)
    recorder.attach(() => ({ pid: 1234, executable: '/bin/codex', lastInputAt: null, lastOutputAt: null, lastEtxAt: null }))
    recorder.spawned()
    recorder.stop('before-quit')
    recorder.exited(-1073741510)
    const report = buildSessionDiagnosticsReport(root, 'test', []) as any
    expect(report.activeSessions).toEqual([])
    const exits = report.files.flatMap((file: any) => file.events).filter((event: any) => event.event === 'process-exit')
    expect(exits).toHaveLength(1)
    expect(exits[0]).toMatchObject({ sessionId: 'session-1', pid: 1234, exitCode: -1073741510, hostStopReason: 'before-quit' })
  })

  it('records source-bridge message submission without storing the prompt or extra context', () => {
    const root = tempRoot()
    let now = 100
    const recorder = new SessionDiagnostics(root, { ...context, prompt: 'private-prompt', env: 'secret-key' } as typeof context, () => now)
    now = 200
    recorder.control('submit_user_message')
    now = 400
    recorder.exited(0)
    expect(events(root).at(-1).sinceInputMs).toBe(200)
    const report = JSON.stringify(buildSessionDiagnosticsReport(root, 'test'))
    expect(report).not.toContain('private-prompt')
    expect(report).not.toContain('secret-key')
  })

  it('captures the bundled source commit at startup', () => {
    const root = tempRoot()
    const kernelRoot = join(root, 'bin', 'aicli')
    mkdirSync(kernelRoot, { recursive: true })
    writeFileSync(join(kernelRoot, 'manifest.json'), JSON.stringify({ entries: { codex: { sourceCommit: 'a'.repeat(40) } } }))
    const recorder = new SessionDiagnostics(root, context)
    recorder.attach(() => ({ pid: 5, executable: join(kernelRoot, 'codex', 'win32-x64', 'codex.exe'), lastInputAt: null, lastOutputAt: null, lastEtxAt: null }))
    recorder.spawned()
    expect(events(root).at(-1).sourceCommit).toBe('a'.repeat(40))
  })

  it('omits extra persisted payload fields from the exported report', () => {
    const root = tempRoot()
    mkdirSync(join(root, 'logs'))
    writeFileSync(join(root, 'logs', 'aicli-lifecycle.jsonl'), JSON.stringify({
      event: 'process-exit', exitCode: 1, prompt: 'private-prompt', env: { token: 'secret-value' }
    }) + '\n')
    const report = JSON.stringify(buildSessionDiagnosticsReport(root, 'test'))
    expect(report).toContain('process-exit')
    expect(report).not.toContain('private-prompt')
    expect(report).not.toContain('secret-value')
  })

  it('rotates large logs while retaining the previous file in the export', () => {
    const root = tempRoot()
    mkdirSync(join(root, 'logs'))
    writeFileSync(join(root, 'logs', 'aicli-lifecycle.jsonl'), ' '.repeat(2 * 1024 * 1024))
    new SessionDiagnostics(root, context)
    const report = buildSessionDiagnosticsReport(root, 'test') as any
    expect(report.files).toHaveLength(2)
    expect(report.files[0].status).toBe('partial')
    expect(report.files[1].events[0].event).toBe('spawn-requested')
  })

  it('keeps write failures non-fatal and makes missing history explicit', () => {
    const root = tempRoot()
    const file = join(root, 'not-a-directory')
    writeFileSync(file, 'x')
    expect(() => {
      const recorder = new SessionDiagnostics(file, context)
      recorder.stop('before-quit')
      recorder.exited(0)
    }).not.toThrow()
    const report = buildSessionDiagnosticsReport(root, 'test') as any
    expect(report.files.every((file: any) => file.status === 'missing')).toBe(true)
    expect((buildSessionDiagnosticsReport(file, 'test') as any).persistenceError).toBeTruthy()
  })
})
