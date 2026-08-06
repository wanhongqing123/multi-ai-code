import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { describe, expect, it } from 'vitest'
import { verifyPackagedCodexBinaries } from './aicli-packaging.mjs'

function createPackagedCodex(root, { platform, arch, includeHost = true }) {
  const resources =
    platform === 'darwin'
      ? join(root, 'Multi-AI Code.app', 'Contents', 'Resources')
      : join(root, 'resources')
  const codexDir = join(
    resources,
    'app.asar.unpacked',
    'bin',
    'aicli',
    'codex',
    `${platform}-${arch}`
  )
  mkdirSync(codexDir, { recursive: true })
  const suffix = platform === 'win32' ? '.exe' : ''
  writeFileSync(join(codexDir, `codex${suffix}`), '')
  if (includeHost) writeFileSync(join(codexDir, `codex-code-mode-host${suffix}`), '')
}

describe('AICLI package verification', () => {
  it.each([
    ['darwin', 'arm64'],
    ['win32', 'x64']
  ])('accepts a %s package containing Codex and its host', (platform, arch) => {
    const root = mkdtempSync(join(tmpdir(), 'aicli-package-'))
    try {
      createPackagedCodex(root, { platform, arch })
      expect(verifyPackagedCodexBinaries(root, { platform, arch })).toHaveLength(2)
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  it('rejects a package that omits codex-code-mode-host', () => {
    const root = mkdtempSync(join(tmpdir(), 'aicli-package-'))
    try {
      createPackagedCodex(root, { platform: 'darwin', arch: 'arm64', includeHost: false })
      expect(() =>
        verifyPackagedCodexBinaries(root, { platform: 'darwin', arch: 'arm64' })
      ).toThrow('codex-code-mode-host')
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })
})
