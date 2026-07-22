import { describe, expect, it } from 'vitest'
import { join } from 'path'
import {
  bundledCliFromCommand,
  bundledPlatformArch,
  describeAicliLaunchCommand,
  resolveAicliCommand,
  resolveBundledCliCommand
} from '../../../electron/aicli/bundledCliResolver.js'

describe('bundledCliResolver', () => {
  it('maps current platform and arch to a stable directory name', () => {
    expect(bundledPlatformArch('darwin', 'arm64')).toBe('darwin-arm64')
    expect(bundledPlatformArch('win32', 'x64')).toBe('win32-x64')
  })

  it('recognizes only bare Codex and OpenCode commands', () => {
    expect(bundledCliFromCommand('codex')).toBe('codex')
    expect(bundledCliFromCommand('"opencode"')).toBe('opencode')
    expect(bundledCliFromCommand('/custom/bin/codex')).toBeNull()
    expect(bundledCliFromCommand('claude')).toBeNull()
    expect(bundledCliFromCommand('my-codex-wrapper')).toBeNull()
  })

  it('resolves bundled Codex when the platform binary exists', () => {
    const root = '/repo/bin/aicli'
    const expected = join(root, 'codex', 'darwin-arm64', 'codex')
    expect(
      resolveBundledCliCommand('codex', {
        platform: 'darwin',
        arch: 'arm64',
        roots: [root],
        existsFile: (path) => path === expected
      })
    ).toBe(expected)
  })

  it('resolves bundled OpenCode when the platform binary exists', () => {
    const root = '/repo/bin/aicli'
    const expected = join(root, 'opencode', 'win32-x64', 'opencode.exe')
    expect(
      resolveBundledCliCommand('opencode', {
        platform: 'win32',
        arch: 'x64',
        roots: [root],
        existsFile: (path) => path === expected
      })
    ).toBe(expected)
  })

  it('does not replace custom commands or Claude', () => {
    const existsFile = () => true
    expect(
      resolveBundledCliCommand('/custom/bin/codex', {
        roots: ['/repo/bin/aicli'],
        existsFile
      })
    ).toBeNull()
    expect(
      resolveBundledCliCommand('claude', {
        roots: ['/repo/bin/aicli'],
        existsFile
      })
    ).toBeNull()
  })

  it('forces codex/opencode to the bundled binary and reports when it is missing', () => {
    const root = '/repo/bin/aicli'
    const codexBundled = join(root, 'codex', 'darwin-arm64', 'codex')
    const opts = { platform: 'darwin' as const, arch: 'arm64' as const, roots: [root] }

    // 裸命令 codex：解析到内置。
    expect(
      resolveAicliCommand('codex', { ...opts, existsFile: (p) => p === codexBundled })
    ).toEqual({ tool: 'codex', label: 'Codex', bundledCommand: codexBundled, bundledMissing: false })

    // 宿主机自定义/PATH 路径 codex：仍强制解析到内置（无视传入路径）。
    expect(
      resolveAicliCommand('/usr/local/bin/codex', { ...opts, existsFile: (p) => p === codexBundled })
    ).toMatchObject({ tool: 'codex', bundledCommand: codexBundled, bundledMissing: false })

    // 内置二进制缺失：置 bundledMissing，调用方据此报错、绝不回退宿主机版本。
    expect(
      resolveAicliCommand('codex', { ...opts, existsFile: () => false })
    ).toEqual({ tool: 'codex', label: 'Codex', bundledCommand: null, bundledMissing: true })
    expect(
      resolveAicliCommand('opencode', { ...opts, existsFile: () => false })
    ).toEqual({ tool: 'opencode', label: 'OpenCode', bundledCommand: null, bundledMissing: true })
  })

  it('leaves claude and other commands unconstrained by the bundled policy', () => {
    for (const command of ['claude', '/custom/bin/claude', 'echo', 'my-codex-wrapper']) {
      expect(resolveAicliCommand(command, { existsFile: () => false })).toEqual({
        tool: null,
        label: null,
        bundledCommand: null,
        bundledMissing: false
      })
    }
  })

  it('describes bundled Codex launch paths for startup visibility', () => {
    const path = '/repo/bin/aicli/codex/darwin-arm64/codex'
    expect(describeAicliLaunchCommand('codex', path, path)).toEqual({
      tool: 'codex',
      label: 'Codex',
      source: 'bundled',
      commandPath: path,
      notice: `当前启动 Codex：内置版本 ${path}`
    })
  })

  it('describes custom OpenCode paths without rewriting them', () => {
    const path = '/custom/bin/opencode'
    expect(describeAicliLaunchCommand(path, path, null)).toMatchObject({
      tool: 'opencode',
      label: 'OpenCode',
      source: 'custom',
      commandPath: path,
      notice: `当前启动 OpenCode：自定义路径 ${path}`
    })
  })

  it('describes PATH fallback when no bundled binary exists', () => {
    const path = '/opt/homebrew/bin/codex'
    expect(describeAicliLaunchCommand('codex', path, null)).toMatchObject({
      tool: 'codex',
      label: 'Codex',
      source: 'path',
      commandPath: path,
      notice: `当前启动 Codex：系统 PATH ${path}`
    })
  })
})
