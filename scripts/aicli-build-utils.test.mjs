import { mkdtempSync, rmSync, statSync, writeFileSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it } from 'vitest'
import { copyExecutable } from './aicli-build-utils.mjs'

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
})
