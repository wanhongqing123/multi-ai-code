import { mkdirSync, readdirSync, rmSync, writeFileSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it } from 'vitest'
import {
  asrPlatformKey,
  parseAsrTargets,
  pruneAsrRuntimeForPlatform
} from './asr-packaging.mjs'

function makeAsrTree() {
  const root = join(tmpdir(), `asr-packaging-${process.pid}-${Date.now()}`)
  for (const dir of ['darwin-arm64', 'win32-x64', 'models']) {
    mkdirSync(join(root, dir), { recursive: true })
    writeFileSync(join(root, dir, 'marker.txt'), dir)
  }
  writeFileSync(join(root, 'README.md'), 'asr')
  return root
}

describe('ASR packaging utilities', () => {
  it('maps electron platform and arch to packaged ASR runtime directory', () => {
    expect(asrPlatformKey('darwin', 'arm64')).toBe('darwin-arm64')
    expect(asrPlatformKey('win32', 'x64')).toBe('win32-x64')
  })

  it('keeps only darwin runtime and models for macOS packages', () => {
    const root = makeAsrTree()
    try {
      const result = pruneAsrRuntimeForPlatform(root, { platform: 'darwin', arch: 'arm64' })
      expect(result.removed).toEqual(['win32-x64'])
      expect(readdirSync(root).sort()).toEqual(['README.md', 'darwin-arm64', 'models'])
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  it('keeps only Windows runtime and models for Windows packages', () => {
    const root = makeAsrTree()
    try {
      const result = pruneAsrRuntimeForPlatform(root, { platform: 'win32', arch: 'x64' })
      expect(result.removed).toEqual(['darwin-arm64'])
      expect(readdirSync(root).sort()).toEqual(['README.md', 'models', 'win32-x64'])
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  it('parses explicit ASR targets for platform-specific packaging', () => {
    expect(parseAsrTargets(['--target', 'darwin-arm64'])).toEqual(['darwin-arm64'])
    expect(parseAsrTargets(['--target=win32-x64'])).toEqual(['win32-x64'])
  })
})
