import { createReadStream, promises as fs } from 'fs'
import { basename, isAbsolute, join } from 'path'
import { createHash } from 'crypto'
import { readTuiHistoryDiagnostics } from './tuiHistoryDiagnostics.js'
import { readCodexDiagnosticEvents } from './codexDiagnostics.js'

const MAX_READ_BYTES = 2 * 1024 * 1024
const MAX_EVENTS = 1000
const WINDOW_MS = 30 * 60 * 1000

// Deliberately omit text, previews, paths, error messages, environment and
// credentials. Remote collection must not export arbitrary log properties.
const ID_FIELDS = ['id', 'ID', 'remoteMessageId', 'callId', 'sessionId', 'taskId', 'replyId', 'messageId', 'partId', 'threadId', 'turnId',
  'eventTaskId', 'eventReplyId', 'sourceCommit', 'onDiskBinarySha256']
const LABEL_FIELDS = ['event', 'kind', 'sourceKind', 'cli', 'status', 'terminalKind',
  'hostStopReason', 'stopReasonRequested', 'spawnErrorCode', 'signal', 'exitCodeHex', 'appVersion', 'type', 'phase', 'delivery', 'onDiskBinaryStatus', 'stage']
const NUMBER_FIELDS = ['createdAt', 'startedAt', 'pid', 'hostPid', 'lifetimeMs', 'lastInputAt',
  'lastOutputAt', 'lastEtxAt', 'stopRequestedAt', 'exitCode', 'textLength', 'inputLength',
  'resolvedLength', 'forwardedChunks', 'code', 'errorCode', 'messageId', 'attempt', 'onDiskBinaryBytes', 'duration_ms', 'samples', 'slow_samples', 'slow_threshold_ms', 'max_ms', 'average_ms', 'late_ms', 'sample_interval_ms', 'visibleLength', 'cellCount', 'replacedCells']

export function diagnosticMetadata(value: unknown, depth = 0): Record<string, unknown> {
  if (depth > 2) return {}
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {}
  const row = value as Record<string, unknown>
  const result: Record<string, unknown> = {}
  for (const key of [...ID_FIELDS, ...LABEL_FIELDS]) {
    const v = row[key]
    if (typeof v === 'string' && /^[a-zA-Z0-9_.:@/-]{1,200}$/.test(v)) result[key] = v
  }
  for (const key of NUMBER_FIELDS) {
    if (typeof row[key] === 'number' && Number.isFinite(row[key])) result[key] = row[key]
  }
  for (const key of ['ok', 'sourceStarted', 'autoReplyToIm', 'sdkReady', 'isReady', 'accepted', 'hasStream', 'replay']) {
    if (typeof row[key] === 'boolean') result[key] = row[key]
  }
  if (Array.isArray(row.candidates)) result.candidates = row.candidates.slice(0, 20).map(value => diagnosticMetadata(value, depth + 1))
  return result
}

async function readTail(path: string): Promise<{ status: string; truncated: boolean; invalidLines: number; rows: Record<string, any>[] }> {
  try {
    const file = await fs.open(path, 'r')
    try {
      const { size } = await file.stat()
      const start = Math.max(0, size - MAX_READ_BYTES)
      const data = Buffer.alloc(Math.min(size, MAX_READ_BYTES))
      const { bytesRead } = await file.read(data, 0, data.length, start)
      let text = data.subarray(0, bytesRead).toString('utf8')
      if (start > 0) text = text.slice(text.indexOf('\n') + 1)
      const rows: Record<string, any>[] = []
      let invalidLines = 0
      for (const line of text.split('\n').filter(Boolean)) {
        try {
          const row = JSON.parse(line)
          if (!row || typeof row !== 'object' || Array.isArray(row)) throw new Error('invalid row')
          rows.push(row)
        } catch { invalidLines++ }
      }
      return { status: invalidLines ? 'partial' : 'ok', truncated: start > 0, invalidLines, rows }
    } finally { await file.close() }
  } catch (error) {
    return { status: (error as NodeJS.ErrnoException).code === 'ENOENT' ? 'missing' : 'unreadable', truncated: false, invalidLines: 0, rows: [] }
  }
}

export interface RemoteDiagnosticsInput {
  root: string; projectId: string; appVersion: string; requestId: string; now?: number
  activeSessions?: object[]
  appDataRoot?: string
  targetRepo?: string
  signal?: AbortSignal
}

async function activeSessionMetadata(sessions: object[], signal?: AbortSignal): Promise<object[]> {
  const cache = new Map<string, Promise<Record<string, unknown>>>()
  const results: object[] = []
  for (const value of sessions.slice(0, 32)) {
    signal?.throwIfAborted()
    const session = value as { executable?: unknown; cli?: unknown }
    const metadata = diagnosticMetadata(value)
    if (typeof session.executable === 'string' && isAbsolute(session.executable) &&
        typeof session.cli === 'string' && session.cli === 'codex' &&
        [session.cli, `${session.cli}.exe`].includes(basename(session.executable).toLowerCase())) {
      const path = session.executable
      if (!cache.has(path) && cache.size < 4) cache.set(path, (async () => {
        try {
          const before = await fs.stat(path)
          if (!before.isFile() || before.size > 512 * 1024 * 1024) return { onDiskBinaryStatus: 'too-large-or-not-file' }
          const hash = createHash('sha256')
          for await (const chunk of createReadStream(path, { signal })) hash.update(chunk)
          const after = await fs.stat(path)
          if (before.size !== after.size || before.mtimeMs !== after.mtimeMs || before.ino !== after.ino) return { onDiskBinaryStatus: 'changed-during-collection' }
          return { onDiskBinaryStatus: 'ok', onDiskBinarySha256: hash.digest('hex'), onDiskBinaryBytes: before.size }
        } catch { return { onDiskBinaryStatus: 'unavailable' } }
      })())
      Object.assign(metadata, await cache.get(path) ?? { onDiskBinaryStatus: 'collection-limit' })
    }
    results.push(metadata)
  }
  return results
}

/** Bound repeated collection requests without ever accepting a caller path. */
export function createRemoteDiagnosticsService() {
  const cache = new Map<string, { at: number; result: Promise<{ ok: true; text: string; attachmentPath: string }> }>()
  const last = new Map<string, number>()
  return async (input: RemoteDiagnosticsInput & { requesterUserId: string }) => {
    const now = input.now ?? Date.now()
    for (const [key, value] of cache) if (now - value.at > 5 * 60_000) cache.delete(key)
    for (const [key, at] of last) if (now - at > 60_000) last.delete(key)
    const owner = JSON.stringify([input.root, input.projectId, input.requesterUserId])
    const key = JSON.stringify([owner, input.requestId])
    const prior = cache.get(key)
    if (prior) return prior.result
    if (now - (last.get(owner) ?? -Infinity) < 30_000 || cache.size >= 64) {
      return { ok: false as const, text: `远程排障请求过于频繁，请稍后重试（${input.requestId}）。` }
    }
    last.set(owner, now)
    const controller = new AbortController()
    let timer: ReturnType<typeof setTimeout> | undefined
    const timeout = new Promise<never>((_resolve, reject) => {
      timer = setTimeout(() => {
        controller.abort()
        reject(new Error('diagnostic collection timed out'))
      }, 25_000)
      timer.unref?.()
    })
    const result = Promise.race([createRemoteDiagnosticsReport({ ...input, signal: controller.signal }), timeout])
      .finally(() => { if (timer) clearTimeout(timer) })
    cache.set(key, { at: now, result })
    return result
  }
}

export async function createRemoteDiagnosticsReport(input: RemoteDiagnosticsInput): Promise<{ ok: true; text: string; attachmentPath: string }> {
  input.signal?.throwIfAborted()
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(input.requestId)) throw new Error('invalid diagnostic request id')
  const now = input.now ?? Date.now()
  const sources = ['remote-im-runtime.log', 'logs/aicli-lifecycle.jsonl.1', 'logs/aicli-lifecycle.jsonl']
  const loaded = await Promise.all(sources.map(name => readTail(join(input.root, name))))
  input.signal?.throwIfAborted()
  const sessions = new Set<string>()
  for (const session of input.activeSessions ?? []) {
    const id = (session as { sessionId?: unknown }).sessionId
    if (typeof id === 'string') sessions.add(id)
  }
  for (const source of loaded) for (const row of source.rows) {
    if (row.projectId === input.projectId) {
      const id = row.sessionId ?? row.detail?.sessionId
      if (typeof id === 'string') sessions.add(id)
    }
  }
  const files = loaded.map((source, index) => {
    const scoped = source.rows.filter(row => {
      const timestamp = row.createdAt
      if (typeof timestamp !== 'number' || timestamp < now - WINDOW_MS || timestamp > now) return false
      // Unmatched route entries may omit projectId. Include them only when a
      // session can be associated with this project by another retained record.
      return row.projectId === input.projectId || (row.projectId == null &&
        sessions.has(row.sessionId ?? row.detail?.sessionId))
    })
    return {
      source: sources[index], status: source.status, truncated: source.truncated || scoped.length > MAX_EVENTS,
      invalidLines: source.invalidLines,
      events: scoped.slice(-MAX_EVENTS).map(row => ({ ...diagnosticMetadata(row), detail: diagnosticMetadata(row.detail) }))
    }
  })
  if (input.appDataRoot && input.targetRepo) {
    const projectSessions = [...(input.activeSessions ?? []), ...loaded.slice(1).flatMap(source => source.rows.filter(row => row.projectId === input.projectId))]
    const homes = projectSessions
      .filter(value => (value as { cli?: string }).cli === 'codex')
      .map(value => (value as { codexHome?: unknown }).codexHome)
      .filter((home): home is string => typeof home === 'string')
    const source = await readCodexDiagnosticEvents({ appDataRoot: input.appDataRoot, targetRepo: input.targetRepo, homes, since: now - WINDOW_MS, now })
    files.push({ ...source, events: source.events.map(value => ({ ...diagnosticMetadata(value), detail: {} })) })
    const history = await readTuiHistoryDiagnostics({ appDataRoot: input.appDataRoot, sessions: projectSessions, since: now - WINDOW_MS, now })
    files.push({ ...history, events: history.events.map(value => ({ ...diagnosticMetadata(value), detail: {} })) })
  }
  const report = {
    schemaVersion: 1, requestId: input.requestId, appVersion: input.appVersion, platform: process.platform,
    timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone,
    electronVersion: process.versions.electron ?? null, exportedAt: now, from: now - WINDOW_MS,
    projectId: input.projectId, files,
    activeSessions: await activeSessionMetadata(input.activeSessions ?? [], input.signal),
    sourceCoverage: { uiPerformance: 'unavailable', aicliOriginalEvents: files.some(file => file.source === 'codex-original-events' && ['ok', 'partial'].includes(file.status)) ? 'see-codex-original-events' : 'unavailable' },
    notes: ['仅含当前项目最近 30 分钟的诊断元数据；不含聊天正文、路径、凭据或环境变量。',
      'missing、partial、truncated 或没有匹配事件均不证明没有故障；可能需要补充现场记录。',
      '本报告由 MultiAICode 宿主生成，不需要运行中的 AI；IM 客户端离线时无法回传。',
      'sourceCommit 是启动时 manifest 的记录；onDiskBinarySha256 是采集时磁盘文件的哈希，不证明旧进程已加载该文件。']
  }
  const dir = join(input.root, 'remote-im-diagnostics')
  input.signal?.throwIfAborted()
  await fs.mkdir(dir, { recursive: true, mode: 0o700 })
  // Generated upload cache only: never enumerate or delete arbitrary account
  // files. Retain recent reports for delivery retries and bound disk growth.
  let retained = 0
  for (const entry of await fs.readdir(dir, { withFileTypes: true })) {
    if (!entry.isFile() || !/^remote-diagnostics-[0-9a-f-]{36}\.json$/.test(entry.name)) continue
    const path = join(dir, entry.name)
    const stat = await fs.stat(path).catch(() => null)
    if (!stat) continue
    if (Date.now() - stat.mtimeMs > 24 * 60 * 60_000) await fs.unlink(path).catch(() => undefined)
    else retained++
  }
  if (retained >= 64) throw new Error('diagnostic upload cache at capacity')
  let encoded = JSON.stringify(report, null, 2)
  while (Buffer.byteLength(encoded) > MAX_READ_BYTES) {
    const largest = [...files].sort((a, b) => b.events.length - a.events.length)[0]
    if (!largest?.events.length) throw new Error('diagnostic metadata too large')
    largest.events.splice(0, Math.max(1, Math.ceil(largest.events.length / 4)))
    largest.truncated = true
    encoded = JSON.stringify(report, null, 2)
  }
  const attachmentPath = join(dir, `remote-diagnostics-${input.requestId}.json`)
  input.signal?.throwIfAborted()
  await fs.writeFile(attachmentPath, encoded, { flag: 'wx', mode: 0o600 })
  return { ok: true, text: `远程排障报告已生成（${input.requestId}）：当前项目最近 30 分钟，仅诊断元数据，不含正文或登录凭据。`, attachmentPath }
}
