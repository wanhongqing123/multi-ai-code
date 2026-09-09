import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { promises as fs, mkdtempSync, mkdirSync, readFileSync, readdirSync, rmSync, symlinkSync, utimesSync, writeFileSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { createHash } from 'crypto'
import { createRemoteDiagnosticsReport, createRemoteDiagnosticsService } from '../../../electron/remote-im/diagnostics.js'
import { executeRemoteImControlCommand } from '../../../electron/remote-im/controlBridge.js'

const requestId = '65de6748-3a4e-4d08-8ffd-bc78e1804ff9'
const now = 1_800_000_000_000
describe('remote diagnostics collection', () => {
  let root: string
  beforeEach(() => { root = mkdtempSync(join(tmpdir(), 'remote-diagnostics-')) })
  afterEach(() => { vi.restoreAllMocks(); rmSync(root, { recursive: true, force: true }) })

  it('produces the same event and session contract consumed by the Qt report fixture', async () => {
    const fixture = JSON.parse(readFileSync(new URL('../../fixtures/remote-diagnostics-report.json', import.meta.url), 'utf8'))
    writeFileSync(join(root, 'remote-im-runtime.log'), fixture.files[0].events
      .map((event: object) => JSON.stringify({ ...event, projectId: fixture.projectId })).join('\n'))
    const output = await createRemoteDiagnosticsReport({ root, projectId: fixture.projectId,
      appVersion: fixture.appVersion, requestId: fixture.requestId, now: fixture.exportedAt,
      activeSessions: fixture.activeSessions })
    const actual = JSON.parse(readFileSync(output.attachmentPath, 'utf8'))
    expect(actual.files[0]).toEqual(fixture.files[0])
    expect(actual.activeSessions).toEqual(fixture.activeSessions)
    expect(actual.exportedAt).toBe(fixture.exportedAt)
    expect(actual.from).toBe(fixture.from)
    expect(actual.projectId).toBe(fixture.projectId)
    expect(actual.sourceCoverage).toEqual(fixture.sourceCoverage)
  })

  it('exports correlated metadata, but not credentials, text or another project', async () => {
    mkdirSync(join(root, 'logs'))
    const entries = [
      { projectId: 'p', createdAt: now - 100, event: 'aicli:route-registered', detail: { sessionId: 's', taskId: 't', replyId: 'r', token: 'TOKEN_SENTINEL', textPreview: 'PRIVATE_TEXT' } },
      { projectId: null, createdAt: now - 90, event: 'aicli:event-unmatched', detail: { sessionId: 's', taskId: 'old', kind: 'assistant_text', textLength: 5, textPreview: 'PRIVATE_TEXT', candidates: [{ taskId: 't', replyId: 'r' }] } },
      { projectId: 'other', createdAt: now - 80, event: 'OTHER_PROJECT_SENTINEL', detail: { sessionId: 'other-session' } },
      { projectId: 'p', createdAt: now - 31 * 60_000, event: 'TOO_OLD_SENTINEL' },
      { projectId: 'p', createdAt: now - 70, event: 'send:resolved', messageId: 12, detail: { code: 0, message: 'PRIVATE_ERROR' } }
    ]
    writeFileSync(join(root, 'remote-im-runtime.log'), entries.map(e => JSON.stringify(e)).join('\n'))
    const output = await createRemoteDiagnosticsReport({ root, projectId: 'p', appVersion: 'test', requestId, now,
      activeSessions: [{ sessionId: 's', sourceCommit: 'abc', executable: '/PRIVATE_PATH/codex', codexHome: '/PRIVATE_HOME' }] })
    const text = readFileSync(output.attachmentPath, 'utf8')
    const report = JSON.parse(text)
    expect(report.requestId).toBe(requestId)
    expect(report.files[0].events).toHaveLength(3)
    expect(report.files[0].events[1].detail).toEqual({ sessionId: 's', taskId: 'old', kind: 'assistant_text', textLength: 5, candidates: [{ taskId: 't', replyId: 'r' }] })
    expect(report.files[0].events[2]).toMatchObject({ messageId: 12, detail: { code: 0 } })
    for (const sentinel of ['TOKEN_SENTINEL', 'PRIVATE_TEXT', 'OTHER_PROJECT_SENTINEL', 'TOO_OLD_SENTINEL', 'PRIVATE_ERROR', 'PRIVATE_PATH', 'PRIVATE_HOME']) expect(text).not.toContain(sentinel)
    expect(report.files[1].status).toBe('missing')
  })

  it('marks malformed input and bounded tails instead of claiming complete logs', async () => {
    writeFileSync(join(root, 'remote-im-runtime.log'), 'x'.repeat(2 * 1024 * 1024 + 1) + '\ninvalid\n' + JSON.stringify({ projectId: 'p', createdAt: now, event: 'send:resolved' }))
    const result = await createRemoteDiagnosticsReport({ root, projectId: 'p', appVersion: 'test', requestId, now })
    const report = JSON.parse(readFileSync(result.attachmentPath, 'utf8'))
    expect(report.files[0]).toMatchObject({ truncated: true, status: 'partial', invalidLines: 1 })
    expect(report.files[0].events).toHaveLength(1)
  })

  it('includes async AICLI item metadata before the IM bridge, not tool arguments or other sessions', async () => {
    const home = join(root, '.codex')
    const dir = join(home, 'sessions', '2026', '09', '09')
    mkdirSync(dir, { recursive: true })
    const targetRepo = join(root, 'repo')
    const entry = { timestamp: new Date(now - 500).toISOString(), type: 'event_msg', payload: {
      type: 'item_completed', thread_id: 'thread-a', turn_id: 'turn-a', item: {
        type: 'AgentMessage', id: 'call-async', phase: 'final_answer', delivery: 'async',
        content: [{ type: 'Text', text: 'QUESTION_SECRET_SENTINEL' }]
      }
    } }
    const meta = (cwd: string) => ({ type: 'session_meta', payload: { id: 'thread-a', cwd, base_instructions: 'INSTRUCTION_SENTINEL' } })
    writeFileSync(join(dir, 'rollout-fixture.jsonl'), [meta(targetRepo), entry].map(row => JSON.stringify(row)).join('\n'))
    writeFileSync(join(dir, 'rollout-other.jsonl'), [meta(join(root, 'other')), { ...entry, payload: { ...entry.payload, thread_id: 'OTHER_THREAD_SENTINEL' } }].map(row => JSON.stringify(row)).join('\n'))
    for (const file of readdirSync(dir)) utimesSync(join(dir, file), now / 1000, now / 1000)
    const result = await createRemoteDiagnosticsReport({ root, appDataRoot: root, targetRepo, projectId: 'p', appVersion: 'test', requestId, now,
      activeSessions: [{ cli: 'codex', sessionId: 's', codexHome: home }] })
    const text = readFileSync(result.attachmentPath, 'utf8')
    const source = JSON.parse(text).files.find((file: any) => file.source === 'codex-original-events')
    expect(source.status).toBe('ok')
    expect(source.events).toEqual([{ createdAt: now - 500, event: 'item_completed', threadId: 'thread-a', turnId: 'turn-a', messageId: 'call-async', phase: 'final_answer', delivery: 'async', kind: 'AgentMessage', detail: {} }])
    for (const secret of ['QUESTION_SECRET_SENTINEL', 'INSTRUCTION_SENTINEL', 'OTHER_THREAD_SENTINEL']) expect(text).not.toContain(secret)
  })

  it('deduplicates a request and bounds repeated collection', async () => {
    const collect = createRemoteDiagnosticsService()
    const input = { root, projectId: 'p', appVersion: 'test', requestId, now, requesterUserId: 'phone' }
    const first = await collect(input)
    expect(await collect(input)).toEqual(first)
    expect(readdirSync(join(root, 'remote-im-diagnostics'))).toHaveLength(1)
    expect((await collect({ ...input, requestId: '75de6748-3a4e-4d08-8ffd-bc78e1804ff9' })).ok).toBe(false)
  })

  it('records an on-disk executable hash without exporting paths or binary contents', async () => {
    const executable = join(root, process.platform === 'win32' ? 'codex.exe' : 'codex')
    const bytes = 'BINARY_CONTENT_SENTINEL'
    writeFileSync(executable, bytes)
    const result = await createRemoteDiagnosticsReport({ root, projectId: 'p', appVersion: 'test', requestId, now,
      activeSessions: [{ cli: 'codex', executable, sessionId: 's', sourceCommit: 'a'.repeat(40), startedAt: now - 1000 }] })
    const text = readFileSync(result.attachmentPath, 'utf8')
    expect(JSON.parse(text).activeSessions[0]).toMatchObject({
      sourceCommit: 'a'.repeat(40), startedAt: now - 1000,
      onDiskBinaryStatus: 'ok', onDiskBinaryBytes: Buffer.byteLength(bytes),
      onDiskBinarySha256: createHash('sha256').update(bytes).digest('hex')
    })
    expect(text).not.toContain(root)
    expect(text).not.toContain(bytes)
  })

  it.each(['direct', 'link'])('does not open rollouts outside the application data root (%s)', async (mode) => {
    const appRoot = join(root, 'app')
    const external = join(root, 'external')
    mkdirSync(appRoot)
    mkdirSync(join(external, 'sessions'), { recursive: true })
    let codexHome = external
    if (mode === 'link') {
      codexHome = join(appRoot, '.codex')
      symlinkSync(external, codexHome, process.platform === 'win32' ? 'junction' : 'dir')
    }
    const open = vi.spyOn(fs, 'open')
    const result = await createRemoteDiagnosticsReport({ root: appRoot, appDataRoot: appRoot, targetRepo: join(root, 'repo'),
      projectId: 'p', requestId, appVersion: 'test', now, activeSessions: [{ cli: 'codex', codexHome }] })
    expect(open.mock.calls.every(([path]) => !String(path).startsWith(external))).toBe(true)
    const report = JSON.parse(readFileSync(result.attachmentPath, 'utf8'))
    expect(report.files.find((file: any) => file.source === 'codex-original-events').status).toBe('unavailable')
    expect(report.sourceCoverage.aicliOriginalEvents).toBe('unavailable')
  })

  it('does not write a report after collection has been aborted', async () => {
    const controller = new AbortController()
    controller.abort()
    await expect(createRemoteDiagnosticsReport({ root, projectId: 'p', requestId, appVersion: 'test', signal: controller.signal })).rejects.toThrow()
    expect(readdirSync(root)).toEqual([])
  })

  it('runs the host collector even without an AI session and rejects path arguments', async () => {
    const executeCommand = vi.fn()
    const createDiagnosticsReport = vi.fn(async (id: string) => createRemoteDiagnosticsReport({ root, projectId: 'p', appVersion: 'test', requestId: id, now }))
    const input = { command: 'diagnostics' as const, sourceKind: 'unknown' as const, session: null, executeCommand, createDiagnosticsReport }
    expect((await executeRemoteImControlCommand({ ...input, args: requestId })).ok).toBe(true)
    expect(createDiagnosticsReport).toHaveBeenCalledWith(requestId)
    expect(executeCommand).not.toHaveBeenCalled()
    createDiagnosticsReport.mockClear()
    expect((await executeRemoteImControlCommand({ ...input, args: requestId + ' /private/file' })).ok).toBe(false)
    expect((await executeRemoteImControlCommand({ ...input, args: requestId.toUpperCase() })).ok).toBe(false)
    expect(createDiagnosticsReport).not.toHaveBeenCalled()
  })

  it('emits failure receipts matching the shared client contract', async () => {
    const fixture = JSON.parse(readFileSync(new URL('../../fixtures/remote-diagnostics-failure-receipts.json', import.meta.url), 'utf8'))
    expect(fixture.requestId).toBe(requestId)
    const expected = (category: string) => fixture.cases.find((row: any) => row.category === category).text
    const input = { command: 'diagnostics' as const, sourceKind: 'unknown' as const, session: null, args: requestId }
    expect((await executeRemoteImControlCommand(input)).text).toBe(expected('service-unavailable'))
    expect((await executeRemoteImControlCommand({ ...input, createDiagnosticsReport: async () => { throw new Error('PRIVATE_SENTINEL') } })).text).toBe(expected('collection-failed'))
    const collect = createRemoteDiagnosticsService()
    const context = { root, projectId: 'p', appVersion: 'test', requesterUserId: 'phone', now }
    await collect({ ...context, requestId: '75de6748-3a4e-4d08-8ffd-bc78e1804ff9' })
    const limited = await executeRemoteImControlCommand({ ...input, createDiagnosticsReport: id => collect({ ...context, requestId: id }) })
    expect(limited.text).toBe(expected('rate-limited'))
  })
})
