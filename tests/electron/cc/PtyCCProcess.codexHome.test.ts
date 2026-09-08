import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mkdtempSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'

const nativeSpawn = vi.hoisted(() => vi.fn())
const fakeBinary = vi.hoisted(() => ({ path: '' }))
vi.mock('module', () => ({ createRequire: () => () => ({ spawn: nativeSpawn }) }))
vi.mock('../../../electron/aicli/bundledCliResolver.js', () => ({
  resolveAicliCommand: () => ({ bundledCommand: fakeBinary.path, bundledMissing: false }),
  describeAicliLaunchCommand: () => null,
  bundledCliMissingMessage: () => 'missing'
}))

import { PtyCCProcess } from '../../../electron/cc/PtyCCProcess.js'
import { codexRuntimeDir, setActiveAccount } from '../../../electron/store/paths.js'
import { SessionDiagnostics, buildSessionDiagnosticsReport } from '../../../electron/cc/sessionDiagnostics.js'

describe('Codex account isolation at the native PTY boundary', () => {
  let root: string
  let repo: string
  beforeEach(() => {
    root = mkdtempSync(join(tmpdir(), 'codex-home-test-'))
    repo = join(root, 'repo')
    mkdirSync(repo)
    fakeBinary.path = join(root, process.platform === 'win32' ? 'codex.exe' : 'codex')
    writeFileSync(fakeBinary.path, '')
    vi.stubEnv('MULTI_AI_ROOT', root)
    vi.stubEnv('CODEX_HOME', join(root, 'global-codex'))
    vi.stubEnv('CODEX_SQLITE_HOME', join(root, 'global-db'))
    vi.stubEnv('CODEX_ANALYTICS_EVENTS_CAPTURE_FILE', join(root, 'global-capture.jsonl'))
    vi.stubEnv('CODEX_CA_CERTIFICATE', join(root, 'enterprise-ca.pem'))
    vi.stubEnv('CODEX_APP_SERVER_MANAGED_CONFIG_PATH', join(root, 'policy.toml'))
    nativeSpawn.mockReset().mockImplementation(() => ({
      pid: 123, onData: vi.fn(), onExit: vi.fn(), write: vi.fn(), kill: vi.fn()
    }))
  })
  afterEach(() => {
    setActiveAccount(null)
    vi.unstubAllEnvs()
    rmSync(root, { recursive: true, force: true })
  })
  function start(account: string, command = 'codex') {
    setActiveAccount(account)
    const proc = new PtyCCProcess({
      cwd: repo, command, args: ['resume', '--last'],
      env: { CODEX_HOME: join(root, 'caller-codex'), Codex_Home: 'another-shared-home', Codex_Sqlite_Home: 'shared-db' }
    })
    proc.start()
    return proc
  }

  it('isolates accounts sharing a repo, retains the same home on restart, and overrides inherited aliases', () => {
    start('account-a')
    start('account-b')
    start('account-a')
    const options = nativeSpawn.mock.calls.map(call => call[2])
    expect(options.map(o => o.env.CODEX_HOME)).toEqual([
      join(root, 'accounts', 'account-a', '.codex'),
      join(root, 'accounts', 'account-b', '.codex'),
      join(root, 'accounts', 'account-a', '.codex')
    ])
    for (const option of options) {
      expect(option.cwd).toBe(repo)
      expect(Object.keys(option.env).filter(k => k.toUpperCase() === 'CODEX_HOME')).toEqual(['CODEX_HOME'])
      expect(Object.keys(option.env).filter(k => k.toUpperCase() === 'CODEX_SQLITE_HOME')).toEqual(['CODEX_SQLITE_HOME'])
      expect(option.env.CODEX_SQLITE_HOME).toBe(option.env.CODEX_HOME)
      expect(option.env.CODEX_ANALYTICS_EVENTS_CAPTURE_FILE).toBeUndefined()
      expect(option.env.CODEX_CA_CERTIFICATE).toBe(join(root, 'enterprise-ca.pem'))
      expect(option.env.CODEX_APP_SERVER_MANAGED_CONFIG_PATH).toBe(join(root, 'policy.toml'))
    }
    expect(readdirSync(repo)).toEqual([])
    // 首次建目录时会写一份可见的默认 config.toml（见 seedAccountCodexConfig）；
    // 除此之外仍然不往账号目录里搞任何东西。
    expect(readdirSync(join(root, 'accounts', 'account-a', '.codex'))).toEqual(['config.toml'])
  })

  it('does not rewrite the global home or import credentials, sessions, or locks', () => {
    const global = join(root, 'global-codex')
    mkdirSync(global)
    const original = '{"latest_version":"test","dismissed_version":null}'
    for (const name of ['version.json', 'auth.json', 'config.toml', 'session.jsonl', 'writer.lock']) {
      writeFileSync(join(global, name), original)
    }
    setActiveAccount('account-a')
    mkdirSync(codexRuntimeDir(), { recursive: true })
    writeFileSync(join(codexRuntimeDir(), 'version.json'), original)
    start('account-a')
    expect(JSON.parse(readFileSync(join(codexRuntimeDir(), 'version.json'), 'utf8')).dismissed_version).toBe('test')
    for (const name of readdirSync(global)) expect(readFileSync(join(global, name), 'utf8')).toBe(original)
    expect(readdirSync(codexRuntimeDir()).sort()).toEqual(['config.toml', 'version.json'])
  })

  it('does not silently fall back to the shared home when creating the account home fails', () => {
    setActiveAccount('account-a')
    mkdirSync(join(root, 'accounts', 'account-a'), { recursive: true })
    writeFileSync(codexRuntimeDir(), 'not a directory')
    expect(() => start('account-a')).toThrow()
    expect(nativeSpawn).not.toHaveBeenCalled()
  })

  it('requires an account for Codex, but leaves other CLIs unchanged', () => {
    setActiveAccount(null)
    expect(() => new PtyCCProcess({ command: 'codex', cwd: repo }).start()).toThrow(/account/)
    expect(nativeSpawn).not.toHaveBeenCalled()
    start('account-a', 'claw')
    expect(nativeSpawn.mock.calls[0][2].env.CODEX_HOME).toBe(join(root, 'caller-codex'))
    expect(readdirSync(repo)).toEqual([])
  })

  it('retains the effective home in real exported lifecycle records', () => {
    const proc = start('account-a')
    const recorder = new SessionDiagnostics(join(root, 'accounts', 'account-a'), {
      sessionId: 's', projectId: 'p', cli: 'codex', appVersion: 'test'
    })
    recorder.attach(() => proc.diagnostics)
    recorder.spawned()
    recorder.exited(1)
    const report = buildSessionDiagnosticsReport(join(root, 'accounts', 'account-a'), 'test') as any
    const exits = report.files.flatMap((file: any) => file.events).filter((e: any) => e.event === 'process-exit')
    expect(exits).toHaveLength(1)
    expect(exits[0].codexHome).toBe(join(root, 'accounts', 'account-a', '.codex'))
  })
})
