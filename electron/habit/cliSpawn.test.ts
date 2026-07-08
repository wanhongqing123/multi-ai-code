import { afterEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { dirname, join } from 'path'
import { tmpdir } from 'os'
import { buildEnvWithPath, resolveCliSpawn, resolveOnPath } from './cliSpawn.js'
import { buildCliArgs } from './generator.js'

const isWindows = process.platform === 'win32'

const tempRoots: string[] = []

afterEach(async () => {
  while (tempRoots.length) {
    const r = tempRoots.pop()
    if (r) await fs.rm(r, { recursive: true, force: true })
  }
})

async function mkBin(name: string): Promise<{ dir: string; full: string }> {
  const dir = await fs.mkdtemp(join(tmpdir(), 'cli-spawn-'))
  tempRoots.push(dir)
  const full = join(dir, name)
  await fs.writeFile(full, '#!/bin/sh\necho hi\n', { mode: 0o755 })
  return { dir, full }
}

describe('resolveOnPath', () => {
  it('returns the absolute path when given an existing absolute path', async () => {
    const { full } = await mkBin(isWindows ? 'tool.exe' : 'tool')
    expect(resolveOnPath(full, '')).toBe(full)
  })

  it('accepts an existing absolute path wrapped in quotes', async () => {
    const { full } = await mkBin(isWindows ? 'tool.exe' : 'tool')
    expect(resolveOnPath(`"${full}"`, '')).toBe(full)
  })

  it('accepts an existing absolute path wrapped in multiple quote layers', async () => {
    const { full } = await mkBin(isWindows ? 'tool.exe' : 'tool')
    expect(resolveOnPath(`'"${full}"'`, '')).toBe(full)
  })

  it('returns null when the absolute path does not exist', () => {
    expect(resolveOnPath('/definitely/missing/foo123', '')).toBeNull()
  })

  it('finds bare names by walking PATH', async () => {
    const { dir, full } = await mkBin(isWindows ? 'mytool.exe' : 'mytool')
    const envPath = `${dir}${isWindows ? ';' : ':'}/nowhere`
    expect(resolveOnPath('mytool', envPath)).toBe(full)
  })

  it('returns null for missing bare name', () => {
    expect(resolveOnPath('zz_does_not_exist_zz', '/tmp')).toBeNull()
  })
})

describe('buildEnvWithPath', () => {
  it('prepends platform-typical install dirs onto PATH', () => {
    const out = buildEnvWithPath({ PATH: '/existing', Path: undefined })
    const key = Object.keys(out).find((k) => k.toLowerCase() === 'path')!
    expect(out[key]).toContain('/existing')
    if (!isWindows) {
      expect(out[key]).toContain('/opt/homebrew/bin')
      expect(out[key]).toContain('/usr/local/bin')
    }
  })

  it('does not duplicate paths that are already present', () => {
    const sep = isWindows ? ';' : ':'
    const seed = isWindows ? 'C:\\users\\me\\AppData\\Roaming\\npm' : '/opt/homebrew/bin'
    const out = buildEnvWithPath({
      PATH: seed,
      Path: seed,
      APPDATA: 'C:\\users\\me\\AppData\\Roaming',
      LOCALAPPDATA: '',
      USERPROFILE: 'C:\\users\\me',
      HOME: '/Users/me'
    })
    const key = Object.keys(out).find((k) => k.toLowerCase() === 'path')!
    const occurrences = out[key].split(sep).filter((p) => p === seed).length
    expect(occurrences).toBe(1)
  })

  it('on Windows, mirrors USERPROFILE into HOME so credential lookups work', () => {
    if (!isWindows) return
    const out = buildEnvWithPath({
      PATH: '',
      Path: '',
      USERPROFILE: 'C:\\Users\\Foo',
      HOME: '/c/Users/Foo'
    })
    expect(out.HOME).toBe('C:\\Users\\Foo')
  })

  it('skips undefined env values', () => {
    const out = buildEnvWithPath({ PATH: '/x', UNDEF: undefined })
    expect(out.UNDEF).toBeUndefined()
  })
})

describe('resolveCliSpawn', () => {
  it('on POSIX, returns the input command unchanged', () => {
    if (isWindows) return
    const r = resolveCliSpawn('claude', ['-p', 'hi'], { PATH: '/usr/bin' })
    expect(r.ok).toBe(true)
    if (r.ok) {
      expect(r.resolved.spawnCommand).toBe('claude')
      expect(r.resolved.spawnArgs).toEqual(['-p', 'hi'])
      expect(r.resolved.shell).toBe(false)
    }
  })

  it('on Windows, errors when the command is not on PATH', () => {
    if (!isWindows) return
    const r = resolveCliSpawn('zz_no_such_tool', [], { Path: 'C:\\nothing' })
    expect(r.ok).toBe(false)
  })

  it('on Windows, wraps .cmd shims via cmd.exe with /d /s /c', async () => {
    if (!isWindows) return
    const { dir, full } = await mkBin('faketool.cmd')
    const r = resolveCliSpawn('faketool', ['-p', 'hi'], { Path: dir })
    expect(r.ok).toBe(true)
    if (r.ok) {
      expect(r.resolved.spawnCommand.toLowerCase()).toContain('cmd')
      expect(r.resolved.spawnArgs[0]).toBe('/d')
      expect(r.resolved.spawnArgs[1]).toBe('/s')
      expect(r.resolved.spawnArgs[2]).toBe('/c')
      expect(r.resolved.spawnArgs[3]).toBe(full)
      expect(r.resolved.spawnArgs.slice(4)).toEqual(['-p', 'hi'])
    }
  })

  it('on Windows, runs .exe directly without cmd.exe wrapping', async () => {
    if (!isWindows) return
    const { dir, full } = await mkBin('faketool.exe')
    const r = resolveCliSpawn('faketool', ['-p', 'hi'], { Path: dir })
    expect(r.ok).toBe(true)
    if (r.ok) {
      expect(r.resolved.spawnCommand).toBe(full)
      expect(r.resolved.spawnArgs).toEqual(['-p', 'hi'])
    }
  })

  it('on Windows, accepts a quoted absolute .exe path', async () => {
    if (!isWindows) return
    const { full } = await mkBin('faketool.exe')
    const r = resolveCliSpawn(`"${full}"`, ['-p', 'hi'], { Path: 'C:\\nothing' })
    expect(r.ok).toBe(true)
    if (r.ok) {
      expect(r.resolved.spawnCommand).toBe(full)
      expect(r.resolved.spawnArgs).toEqual(['-p', 'hi'])
    }
  })

  it('describes custom Codex launch paths for user-visible diagnostics', async () => {
    const { full } = await mkBin(isWindows ? 'codex.exe' : 'codex')
    const r = resolveCliSpawn(full, ['exec', 'hi'], { PATH: '', Path: '' })
    expect(r.ok).toBe(true)
    if (r.ok) {
      expect(r.resolved.launchNotice).toBe(`当前启动 Codex：自定义路径 ${full}`)
    }
  })

  it('on POSIX, does not display PATH fallback when a custom Codex path is configured', () => {
    if (isWindows) return
    const custom = '/custom/bin/codex'
    const r = resolveCliSpawn(custom, ['exec', 'hi'], {
      PATH: '/Users/hongqingwan/.real/.bin/node/bin',
      Path: ''
    })
    expect(r.ok).toBe(true)
    if (r.ok) {
      expect(r.resolved.spawnCommand).toBe(custom)
      expect(r.resolved.launchNotice).toBe(`当前启动 Codex：自定义路径 ${custom}`)
    }
  })

  it('on Windows, falls back to PATH shim when a stale quoted claude.exe path no longer exists', async () => {
    if (!isWindows) return
    const { dir, full } = await mkBin('claude.cmd')
    const r = resolveCliSpawn(`'"C:\\missing\\claude.exe"'`, ['-p', 'hi'], { Path: dir })
    expect(r.ok).toBe(true)
    if (r.ok) {
      expect(r.resolved.spawnCommand.toLowerCase()).toContain('cmd')
      expect(r.resolved.spawnArgs[3]).toBe(full)
      expect(r.resolved.spawnArgs.slice(4)).toEqual(['-p', 'hi'])
    }
  })

  it('on Windows, prefers the real claude native binary from the npm wrapper package when bin/claude.exe is missing', async () => {
    if (!isWindows) return
    const dir = await fs.mkdtemp(join(tmpdir(), 'claude-wrapper-'))
    tempRoots.push(dir)
    const wrapperCmd = join(dir, 'claude.cmd')
    await fs.writeFile(wrapperCmd, '@echo off\r\n')
    const nativeExe = join(
      dir,
      'node_modules',
      '@anthropic-ai',
      'claude-code',
      'node_modules',
      '@anthropic-ai',
      'claude-code-win32-x64',
      'claude.exe'
    )
    await fs.mkdir(dirname(nativeExe), { recursive: true })
    await fs.writeFile(nativeExe, 'binary')

    const staleExe = join(
      dir,
      'node_modules',
      '@anthropic-ai',
      'claude-code',
      'bin',
      'claude.exe'
    )
    const r = resolveCliSpawn(`'"${staleExe}"'`, ['-p', 'hi'], { Path: dir })
    expect(r.ok).toBe(true)
    if (r.ok) {
      expect(r.resolved.spawnCommand).toBe(nativeExe)
      expect(r.resolved.spawnArgs).toEqual(['-p', 'hi'])
    }
  })

  it('always returns shell:false so prompt content is never shell-parsed', () => {
    const r = resolveCliSpawn('echo', ['hi'], { PATH: '/usr/bin', Path: '' })
    if (r.ok) expect(r.resolved.shell).toBe(false)
  })
})

describe('buildCliArgs (cross-platform argument shape)', () => {
  it('claude uses -p flag for one-shot non-interactive mode', () => {
    const { cmd, args } = buildCliArgs({ ai_cli: 'claude' }, 'hello world')
    expect(cmd).toBe('claude')
    expect(args[0]).toBe('-p')
    expect(args[args.length - 1]).toBe('hello world')
  })

  it('codex uses exec subcommand for one-shot mode', () => {
    const { cmd, args } = buildCliArgs({ ai_cli: 'codex' }, 'hello world')
    expect(cmd).toBe('codex')
    expect(args[0]).toBe('exec')
    expect(args.slice(1, 3)).toEqual(['-c', 'model_context_window=1000000'])
    expect(args[args.length - 1]).toBe('hello world')
  })

  it('opencode uses run subcommand for one-shot mode', () => {
    const { cmd, args } = buildCliArgs({ ai_cli: 'opencode' }, 'hello world')
    expect(cmd).toBe('opencode')
    expect(args).toEqual([
      'run',
      '--dangerously-skip-permissions',
      'hello world'
    ])
  })

  it('defaults to Codex when no AI CLI is configured', () => {
    const { cmd, args } = buildCliArgs({}, 'hello world')
    expect(cmd).toBe('codex')
    expect(args[0]).toBe('exec')
    expect(args.slice(1, 3)).toEqual(['-c', 'model_context_window=1000000'])
  })

  it('preserves user-supplied binary override', () => {
    const { cmd } = buildCliArgs(
      { ai_cli: 'claude', command: '/custom/path/claude' },
      'hi'
    )
    expect(cmd).toBe('/custom/path/claude')
  })

  it('appends user-supplied extra args before the prompt', () => {
    const { args } = buildCliArgs(
      { ai_cli: 'claude', args: ['--model', 'sonnet'] },
      'hi'
    )
    expect(args).toEqual(['-p', '--model', 'sonnet', 'hi'])
  })

  it('does not duplicate codex context window config in one-shot mode', () => {
    const { args } = buildCliArgs(
      { ai_cli: 'codex', args: ['-c', 'model_context_window=272000'] },
      'hi'
    )
    expect(args).toEqual(['exec', '-c', 'model_context_window=272000', 'hi'])
  })

  it('does not duplicate opencode permission bypass in one-shot mode', () => {
    const { args } = buildCliArgs(
      { ai_cli: 'opencode', args: ['--auto'] },
      'hi'
    )
    expect(args).toEqual(['run', '--auto', 'hi'])
  })
})
