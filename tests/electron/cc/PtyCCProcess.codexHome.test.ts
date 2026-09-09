import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { existsSync, mkdtempSync, mkdirSync, readFileSync, readdirSync, rmSync, symlinkSync, writeFileSync } from 'fs'
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

describe('Codex home selection at the native PTY boundary', () => {
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

  // The user asked for one login and one history across accounts, so the home is
  // deliberately shared. It still must not be the caller's, the inherited
  // shell's, or the host's own ~/.codex, and it must never land in the repo.
  it('gives every account the same home, and overrides inherited aliases', () => {
    start('account-a')
    start('account-b')
    start('account-a')
    const options = nativeSpawn.mock.calls.map(call => call[2])
    const shared = join(root, '.codex')
    expect(options.map(o => o.env.CODEX_HOME)).toEqual([shared, shared, shared])
    expect(options[0].env.CODEX_HOME).not.toBe(join(root, 'caller-codex'))
    expect(options[0].env.CODEX_HOME).not.toBe(join(root, 'global-codex'))
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
    expect(readdirSync(shared)).toEqual([])
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
    expect(readdirSync(codexRuntimeDir())).toEqual(['version.json'])
  })

  it('does not silently fall back when creating the shared home fails', () => {
    setActiveAccount('account-a')
    writeFileSync(codexRuntimeDir(), 'not a directory')
    expect(() => start('account-a')).toThrow()
    expect(nativeSpawn).not.toHaveBeenCalled()
  })

  it('requires an account for Codex, but leaves other CLIs unchanged', () => {
    setActiveAccount(null)
    expect(() => new PtyCCProcess({ command: 'codex', cwd: repo }).start()).toThrow(/account/)
    expect(nativeSpawn).not.toHaveBeenCalled()
    start('account-a', 'opencode')
    expect(nativeSpawn.mock.calls[0][2].env.CODEX_HOME).toBe(join(root, 'caller-codex'))
    expect(readdirSync(repo)).toEqual([])
  })

  it('rejects a data root that would use the real host home location before writing', () => {
    const fakeSystemHome = join(root, 'system-user')
    const forbidden = join(fakeSystemHome, '.codex')
    mkdirSync(forbidden, { recursive: true })
    writeFileSync(join(forbidden, 'version.json'), '{"latest_version":"keep"}')
    vi.stubEnv(process.platform === 'win32' ? 'USERPROFILE' : 'HOME', fakeSystemHome)
    vi.stubEnv('MULTI_AI_ROOT', fakeSystemHome)
    expect(() => start('account-a')).toThrow(/host global/)
    expect(nativeSpawn).not.toHaveBeenCalled()
    expect(readFileSync(join(forbidden, 'version.json'), 'utf8')).toBe('{"latest_version":"keep"}')
    expect(readdirSync(forbidden)).toEqual(['version.json'])
  })

  it('rejects shared state inside the opened repository before creating it', () => {
    vi.stubEnv('MULTI_AI_ROOT', repo)
    expect(() => start('account-a')).toThrow(/outside the target repository/)
    expect(nativeSpawn).not.toHaveBeenCalled()
    expect(readdirSync(repo)).toEqual([])
    vi.stubEnv('MULTI_AI_ROOT', join(repo, 'nested-data'))
    expect(() => start('account-a')).toThrow(/outside the target repository/)
    expect(existsSync(join(repo, 'nested-data'))).toBe(false)
  })

  it.each(['direct', 'linked ancestor'])('rejects descendants of the host global home (%s) before writing', (mode) => {
    const fakeSystemHome = join(root, 'system-user')
    const forbidden = join(fakeSystemHome, '.codex')
    mkdirSync(forbidden, { recursive: true })
    writeFileSync(join(forbidden, 'auth.json'), 'fixture-must-stay-unchanged')
    vi.stubEnv(process.platform === 'win32' ? 'USERPROFILE' : 'HOME', fakeSystemHome)
    let ancestor = forbidden
    if (mode === 'linked ancestor') {
      ancestor = join(root, 'alias')
      symlinkSync(forbidden, ancestor, process.platform === 'win32' ? 'junction' : 'dir')
    }
    vi.stubEnv('MULTI_AI_ROOT', join(ancestor, 'nested-state'))
    expect(() => start('account-a')).toThrow(/host global/)
    expect(nativeSpawn).not.toHaveBeenCalled()
    expect(readdirSync(forbidden)).toEqual(['auth.json'])
    expect(readFileSync(join(forbidden, 'auth.json'), 'utf8')).toBe('fixture-must-stay-unchanged')
  })

  it('allows a sibling whose name merely shares the global home prefix', () => {
    const fakeSystemHome = join(root, 'system-user')
    vi.stubEnv(process.platform === 'win32' ? 'USERPROFILE' : 'HOME', fakeSystemHome)
    const dataRoot = join(fakeSystemHome, '.codex-app-data')
    vi.stubEnv('MULTI_AI_ROOT', dataRoot)
    start('account-a')
    expect(nativeSpawn).toHaveBeenCalledOnce()
    expect(nativeSpawn.mock.calls[0][2].env.CODEX_HOME).toBe(join(dataRoot, '.codex'))
    expect(existsSync(join(fakeSystemHome, '.codex'))).toBe(false)
  })

  it('rejects a shared-home symlink or junction into the host global home', () => {
    const fakeSystemHome = join(root, 'system-user')
    const forbidden = join(fakeSystemHome, '.codex')
    mkdirSync(forbidden, { recursive: true })
    vi.stubEnv(process.platform === 'win32' ? 'USERPROFILE' : 'HOME', fakeSystemHome)
    symlinkSync(forbidden, join(root, '.codex'), process.platform === 'win32' ? 'junction' : 'dir')
    expect(() => start('account-a')).toThrow(/host global/)
    expect(nativeSpawn).not.toHaveBeenCalled()
    expect(readdirSync(forbidden)).toEqual([])
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
    // Diagnostics must record the home actually used, which is now the shared one.
    expect(exits[0].codexHome).toBe(join(root, '.codex'))
  })
})
