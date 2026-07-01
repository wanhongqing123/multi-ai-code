import { describe, expect, it, vi } from 'vitest'
import { extractZipArchive } from './archive.mjs'

describe('ASR archive extraction', () => {
  it('falls back to tar when unzip is unavailable', async () => {
    const run = vi.fn(async (command) => {
      if (command === 'unzip') {
        const error = new Error('spawn unzip ENOENT')
        error.code = 'ENOENT'
        throw error
      }
    })

    await extractZipArchive('runtime.zip', 'runtime', { run })

    expect(run).toHaveBeenCalledWith('unzip', ['-q', 'runtime.zip', '-d', 'runtime'])
    expect(run).toHaveBeenCalledWith('tar', ['-xf', 'runtime.zip', '-C', 'runtime'])
  })
})
