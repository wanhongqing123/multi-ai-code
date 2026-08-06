import { mkdtempSync, rmSync, statSync, writeFileSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it } from 'vitest'
import {
  binaryName,
  copyExecutable,
  normalizeManifestEntry,
  resolveBunExecutable,
  resolvePythonCommand,
  rustTargetForPlatform,
  stripArgsForPlatform,
  stripReleaseExecutable
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
    expect(calls).toEqual([['strip', ['-S', '-x', '/tmp/codex']]])
    expect(stripReleaseExecutable('/tmp/codex.exe', { platform: 'win32', runCommand: () => {} })).toBe(
      false
    )
  })
})
