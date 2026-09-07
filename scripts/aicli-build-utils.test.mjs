import { mkdtempSync, rmSync, statSync, writeFileSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it } from 'vitest'
import {
  binaryName,
  copyExecutable,
  normalizeManifestEntry,
  requireVersion,
  resolveBunExecutable,
  resolvePythonCommand,
  rustTargetForPlatform,
  stripArgsForPlatform,
  stripReleaseExecutable,
  assertExecutableRuns
} from './aicli-build-utils.mjs'

describe('AICLI build utilities', () => {
  it('replaces an existing executable instead of overwriting the same inode', () => {
    const dir = mkdtempSync(join(tmpdir(), 'aicli-copy-'))
    try {
      const source = join(dir, 'source')
      const destination = join(dir, 'destination')
      writeFileSync(source, 'new executable')
      writeFileSync(destination, 'old executable')

      const before = statSync(destination)
      copyExecutable(source, destination)
      const after = statSync(destination)

      if (process.platform !== 'win32') {
        expect(after.ino).not.toBe(before.ino)
      }
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  it('resolves the real bun exe from npm global install on Windows', () => {
    const appData = 'C:\\Users\\tester\\AppData\\Roaming'
    const realExe = join(appData, 'npm', 'node_modules', 'bun', 'bin', 'bunx.exe')
    const resolved = resolveBunExecutable('bunx', {
      platform: 'win32',
      env: { APPDATA: appData },
      exists: (path) => path === realExe
    })
    expect(resolved).toBe(realExe)
  })

  it('prefers the official bun installer location when both exist', () => {
    const resolved = resolveBunExecutable('bun', {
      platform: 'win32',
      env: {
        BUN_INSTALL: 'D:\\bun',
        APPDATA: 'C:\\Users\\tester\\AppData\\Roaming'
      },
      exists: () => true
    })
    expect(resolved).toBe(join('D:\\bun', 'bin', 'bun.exe'))
  })

  it('falls back to the bare command when no candidate exists or off Windows', () => {
    expect(
      resolveBunExecutable('bun', {
        platform: 'win32',
        env: { APPDATA: 'C:\\Users\\tester\\AppData\\Roaming' },
        exists: () => false
      })
    ).toBe('bun')
    expect(resolveBunExecutable('bun', { platform: 'darwin' })).toBe('bun')
  })

  it('selects platform-specific strip flags for release binaries', () => {
    expect(stripArgsForPlatform('darwin')).toEqual(['-S', '-x'])
    expect(stripArgsForPlatform('linux')).toEqual(['--strip-unneeded'])
    expect(stripArgsForPlatform('win32')).toBeNull()
  })

  it('uses platform-specific Codex binary names and Rust targets', () => {
    expect(binaryName('codex-code-mode-host', 'darwin')).toBe('codex-code-mode-host')
    expect(binaryName('codex-code-mode-host', 'win32')).toBe('codex-code-mode-host.exe')
    expect(rustTargetForPlatform('darwin', 'arm64')).toBe('aarch64-apple-darwin')
    expect(rustTargetForPlatform('win32', 'x64')).toBe('x86_64-pc-windows-msvc')
  })

  it('records Codex helper binaries as portable manifest paths', () => {
    expect(
      normalizeManifestEntry(
        {
          tool: 'codex',
          binaryPath: '/repo/bin/aicli/codex/win32-x64/codex.exe',
          helperPaths: {
            codeModeHost: '/repo/bin/aicli/codex/win32-x64/codex-code-mode-host.exe'
          }
        },
        '/repo'
      )
    ).toEqual({
      tool: 'codex',
      binaryPath: 'bin/aicli/codex/win32-x64/codex.exe',
      helperPaths: {
        codeModeHost: 'bin/aicli/codex/win32-x64/codex-code-mode-host.exe'
      }
    })
  })

  it('falls back to the Windows py launcher for the V8 resolver', () => {
    const calls = []
    const result = resolvePythonCommand({
      platform: 'win32',
      spawn: (command, args) => {
        calls.push([command, args])
        return command === 'py' ? { status: 0 } : { status: 1 }
      }
    })
    expect(result).toEqual({ command: 'py', prefixArgs: ['-3'] })
    expect(calls).toEqual([
      ['python', ['--version']],
      ['py', ['-3', '--version']]
    ])
  })

  it('strips release executables only on supported platforms', () => {
    const calls = []
    const stripped = stripReleaseExecutable('/tmp/codex', {
      platform: 'darwin',
      runCommand: (command, args) => calls.push([command, args])
    })

    expect(stripped).toBe(true)
    expect(calls).toEqual([
      ['strip', ['-S', '-x', '/tmp/codex']],
      ['codesign', ['--force', '--sign', '-', '/tmp/codex']]
    ])

    const linuxCalls = []
    expect(
      stripReleaseExecutable('/tmp/codex', {
        platform: 'linux',
        runCommand: (command, args) => linuxCalls.push([command, args])
      })
    ).toBe(true)
    expect(linuxCalls).toEqual([['strip', ['--strip-unneeded', '/tmp/codex']]])

    expect(stripReleaseExecutable('/tmp/codex.exe', { platform: 'win32', runCommand: () => {} })).toBe(
      false
    )
  })

  it('requires a runnable build artifact before writing its version to the manifest', () => {
    expect(
      requireVersion('/tmp/codex', {
        spawn: () => ({ status: 0, stdout: 'codex-cli 1.2.3\n', stderr: '' })
      })
    ).toBe('codex-cli 1.2.3')

    expect(() =>
      requireVersion('/tmp/broken', {
        spawn: () => ({ status: 137, stdout: '', stderr: '' })
      })
    ).toThrow(/无法执行 --version/)
  })

  // 这一组钉的是 2026-09-07 v0.1.71 出包时 mac 侧发现的真实缺陷：
  // macOS 上 `strip -S -x` 后的 Rust 产物曾被 AMFI 以 CT signature issue 拒绝——
  // 文件在、体积正常、codesign --verify 也过，但一跑就被 SIGKILL、stdout/stderr
  // 全空。构建脚本原先只在 copy/strip **之前**验过原始二进制，之后仅查存在
  // 与大小，签名异常能一路带进安装包。
  describe('assertExecutableRuns', () => {
    const fakeSpawn = (result) => () => result

    it('rejects a binary killed by a signal（mac strip 破坏的真实形状）', () => {
      expect(() =>
        assertExecutableRuns('/bin/claw', {
          spawn: fakeSpawn({ status: null, signal: 'SIGKILL', stdout: '', stderr: '' })
        })
      ).toThrow(/损坏/)
    })

    it('rejects a binary that runs but prints nothing at all', () => {
      expect(() =>
        assertExecutableRuns('/bin/claw', {
          spawn: fakeSpawn({ status: 137, signal: null, stdout: '', stderr: '' })
        })
      ).toThrow(/没有任何输出/)
    })

    it('rejects a binary that cannot be spawned', () => {
      expect(() =>
        assertExecutableRuns('/bin/claw', {
          spawn: fakeSpawn({ error: new Error('ENOENT') })
        })
      ).toThrow(/无法执行/)
    })

    // 关键的反向锚点：判据必须宽松到不误伤 codex-code-mode-host——
    // 它不认 --version，会以 exit 2 报 unexpected argument，
    // 而那恰恰证明它跑起来并解析了参数。收紧成「必须 exit 0」会把它判死。
    it('accepts a non-zero exit as long as the binary produced output', () => {
      expect(
        assertExecutableRuns('/bin/codex-code-mode-host', {
          spawn: fakeSpawn({
            status: 2,
            signal: null,
            stdout: '',
            stderr: "error: unexpected argument '--version' found"
          })
        })
      ).toContain('unexpected argument')
    })

    it('accepts a healthy binary', () => {
      expect(
        assertExecutableRuns('/bin/claw', {
          spawn: fakeSpawn({ status: 0, signal: null, stdout: 'Claw Code Version 0.1.3', stderr: '' })
        })
      ).toContain('0.1.3')
    })
  })
})
