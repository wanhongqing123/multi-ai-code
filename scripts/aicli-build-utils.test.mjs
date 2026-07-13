import { mkdtempSync, rmSync, statSync, writeFileSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it } from 'vitest'
import {
  copyExecutable,
  resolveBunExecutable,
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
