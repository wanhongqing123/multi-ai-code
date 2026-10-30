import { mkdtemp, rm } from 'fs/promises'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it, vi } from 'vitest'
import { retryRemoteImImage } from '../../../electron/remote-im/imageRetry.js'
import type { RemoteImMessage } from '../../../electron/remote-im/types.js'

describe('remote IM image retry', () => {
  it('recovers only the matching failed message and refuses a changed account', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'im-retry-'))
    const message = {
      id: 7, projectId: 'p1', direction: 'incoming', status: 'failed', kind: 'image',
      remoteMessageId: 'remote-7',
      attachment: { type: 'image', remoteUrl: 'https://example.test/img.png', localPath: null }
    } as RemoteImMessage
    const fetchImpl = vi.fn(async () => ({
      ok: true, status: 200, arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer
    }))
    const restore = vi.fn(() => message)
    const deps = { rootDir, fetchImpl, listById: () => message, restore, isAccountCurrent: () => true }
    try {
      await expect(retryRemoteImImage('other', 7, deps)).rejects.toThrow()
      expect(fetchImpl).not.toHaveBeenCalled()
      await expect(retryRemoteImImage('p1', 7, { ...deps, isAccountCurrent: () => false })).rejects.toThrow()
      expect(restore).not.toHaveBeenCalled()
      await retryRemoteImImage('p1', 7, deps)
      expect(restore).toHaveBeenCalledOnce()
      expect(restore).toHaveBeenCalledWith('p1', 7, 'https://example.test/img.png',
        expect.objectContaining({ localPath: expect.any(String) }))
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })
})
