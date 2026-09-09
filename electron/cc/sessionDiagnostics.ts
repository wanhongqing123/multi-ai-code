import { appendFileSync, existsSync, mkdirSync, readFileSync, renameSync, statSync, unlinkSync } from 'fs'
import { basename, dirname, join, resolve } from 'path'
import { release } from 'os'

export type HostStopReason = 'cc:kill' | 'cc:kill-all' | 'project-delete'
  | 'project-target-changed' | 'window-all-closed' | 'before-quit'

export interface PtyDiagnosticState {
  codexHome?: string | null
  pid: number | null
  executable: string | null
  lastInputAt: number | null
  lastOutputAt: number | null
  lastEtxAt: number | null
}

const FILE_NAME = 'aicli-lifecycle.jsonl'
const MAX_BYTES = 2 * 1024 * 1024
const EXPORT_FIELDS = [
  'sessionId', 'projectId', 'cli', 'appVersion', 'sourceCommit', 'hostPid', 'pid',
  'executable', 'codexHome', 'startedAt', 'lifetimeMs', 'lastInputAt', 'sinceInputMs',
  'lastOutputAt', 'lastEtxAt', 'lastControl', 'lastControlAt', 'hostStopReason',
  'stopRequestedAt', 'stopReasonRequested', 'requestedProjectId', 'event', 'createdAt', 'exitCode', 'signal', 'exitCodeHex',
  'spawnErrorCode'
] as const
// Preserve evidence of earlier write gaps for this host run, scoped to the account.
// A later successful write cannot recover the missing lifecycle records.
const persistenceErrors = new Map<string, string>()

function appendEvent(root: string, event: object): void {
  // Only low-frequency lifecycle/control events touch disk. Synchronous append
  // preserves the stop-request record before kill/quit, without changing either.
  try {
    const dir = join(root, 'logs')
    mkdirSync(dir, { recursive: true })
    const file = join(dir, FILE_NAME)
    if (existsSync(file) && statSync(file).size >= MAX_BYTES) {
      const previous = `${file}.1`
      if (existsSync(previous)) unlinkSync(previous)
      renameSync(file, previous)
    }
    appendFileSync(file, `${JSON.stringify(event)}\n`, { encoding: 'utf8', mode: 0o600 })
  } catch (error) {
    // Never turn a diagnostic failure into a terminal failure.
    persistenceErrors.set(resolve(root), (error as NodeJS.ErrnoException).code ?? 'write-failed')
  }
}

export class SessionDiagnostics {
  private readonly startedAt: number
  private stopReason: HostStopReason | null = null
  private stopRequestedAt: number | null = null
  private lastControl: string | null = null
  private lastControlAt: number | null = null
  private submittedAt: number | null = null
  private sourceCommit: string | null = null
  private readPty: () => PtyDiagnosticState | undefined = () => undefined

  constructor(
    private readonly root: string,
    private readonly context: { sessionId: string; projectId: string; cli: string; appVersion: string },
    private readonly now: () => number = Date.now
  ) {
    this.startedAt = now()
    this.record('spawn-requested')
  }

  attach(readPty: () => PtyDiagnosticState | undefined): void { this.readPty = readPty }

  snapshot(): object {
    const io = this.readPty()
    const lastInputAt = Math.max(io?.lastInputAt ?? 0, this.submittedAt ?? 0) || null
    return {
      sessionId: this.context.sessionId, projectId: this.context.projectId,
      cli: this.context.cli, appVersion: this.context.appVersion,
      sourceCommit: this.sourceCommit,
      hostPid: process.pid, pid: io?.pid ?? null,
      codexHome: io?.codexHome ?? null,
      executable: io?.executable
        ? this.context.cli === 'custom' ? basename(io.executable) : io.executable
        : null,
      startedAt: this.startedAt, lifetimeMs: Math.max(0, this.now() - this.startedAt),
      lastInputAt, sinceInputMs: lastInputAt === null ? null : Math.max(0, this.now() - lastInputAt),
      lastOutputAt: io?.lastOutputAt ?? null, lastEtxAt: io?.lastEtxAt ?? null,
      lastControl: this.lastControl, lastControlAt: this.lastControlAt,
      hostStopReason: this.stopReason, stopRequestedAt: this.stopRequestedAt
    }
  }

  control(command: string): void {
    // Callers pass only the protocol command name, never its arguments/text/token.
    this.lastControl = command
    this.lastControlAt = this.now()
    if (command === 'submit_user_message') this.submittedAt = this.now()
    this.record('control-requested')
  }

  stop(reason: HostStopReason, requestedProjectId?: string): void {
    this.stopReason ??= reason
    this.stopRequestedAt ??= this.now()
    this.record('host-stop-requested', {
      stopReasonRequested: reason, requestedProjectId: requestedProjectId ?? null
    })
  }

  spawned(): void {
    const executable = this.readPty()?.executable
    if (executable && ['codex', 'opencode'].includes(this.context.cli)) {
      try {
        const file = join(dirname(dirname(dirname(executable))), 'manifest.json')
        if (statSync(file).size <= 64 * 1024) {
          const manifest = JSON.parse(readFileSync(file, 'utf8'))
          const commit = manifest?.entries?.[this.context.cli]?.sourceCommit
          if (typeof commit === 'string' && /^[a-f0-9]{40}$/i.test(commit)) this.sourceCommit = commit
        }
      } catch { /* An unavailable manifest must not block startup. */ }
    }
    this.record('spawned')
  }
  spawnFailed(error?: unknown): void {
    const code = error && typeof error === 'object' && 'code' in error ? error.code : null
    // Error messages may contain commands, paths or credentials. Keep only a
    // bounded OS-style error code; unknown failures remain explicitly unknown.
    const spawnErrorCode = typeof code === 'number' && Number.isFinite(code)
      ? code
      : typeof code === 'string' && /^[A-Z][A-Z0-9_]{0,63}$/.test(code) ? code : null
    this.record('spawn-failed', { spawnErrorCode })
  }
  exited(exitCode: number | null, signal?: number | string | null): void {
    this.record('process-exit', {
      exitCode, signal: signal ?? null,
      exitCodeHex: exitCode === null ? null : `0x${(exitCode >>> 0).toString(16).padStart(8, '0')}`
    })
  }

  private record(event: string, fields: object = {}): void {
    appendEvent(this.root, { ...this.snapshot(), ...fields, event, createdAt: this.now() })
  }
}

export function buildSessionDiagnosticsReport(root: string, appVersion: string, activeSessions: object[] = []): object {
  const files = [`${FILE_NAME}.1`, FILE_NAME].map(name => {
    const file = join(root, 'logs', name)
    try {
      if (statSync(file).size > MAX_BYTES + 64 * 1024) return { name, status: 'too-large', events: [] }
      let invalidLines = 0
      const events: unknown[] = []
      for (const line of readFileSync(file, 'utf8').split('\n').filter(Boolean)) {
        try {
          const row = JSON.parse(line)
          if (!row || typeof row !== 'object' || typeof row.event !== 'string') {
            invalidLines++
            continue
          }
          // Keep the export metadata-only even if an older/newer writer adds fields.
          events.push(Object.fromEntries(EXPORT_FIELDS.filter(key => key in row).map(key => [key, row[key]])))
        } catch { invalidLines++ }
      }
      return { name, status: invalidLines ? 'partial' : 'ok', invalidLines, events }
    } catch (error) {
      return { name, status: (error as NodeJS.ErrnoException).code === 'ENOENT' ? 'missing' : 'unreadable', events: [] }
    }
  })
  return {
    schemaVersion: 1, exportedAt: Date.now(), appVersion, account: basename(root),
    platform: process.platform, arch: process.arch, osRelease: release(),
    electronVersion: process.versions.electron ?? null, nodeVersion: process.versions.node,
    persistenceError: persistenceErrors.get(resolve(root)) ?? null, activeSessions, files,
    notes: [
      'Only records collected by versions with lifecycle logging are available; missing history is not proof of no incident.',
      'A null hostStopReason means no host stop request was recorded, not proof of an external kill.',
      'hostStopReason/stopRequestedAt retain the first stop request; stopReasonRequested/createdAt identify each individual stop request.',
      'persistenceError is the last write failure observed for this account during this host run; it may remain after writes recover because earlier records may be missing.',
      'Input/output text, prompts, environment values, credentials and control arguments are not recorded.',
      'Input/control timestamps describe host writes or requests, not a CLI acknowledgement.',
      'Current session snapshots describe export time, not an earlier incident.'
    ]
  }
}
