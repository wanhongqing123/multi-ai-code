import { mkdtemp, readFile, rm } from 'fs/promises'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it } from 'vitest'
import { cacheRemoteImImage } from './imageCache.js'

describe('remote IM image cache', () => {
  it('downloads a remote image into a safe deterministic cache path', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-image-cache-'))
    try {
      const result = await cacheRemoteImImage({
        rootDir,
        projectId: 'project:/1',
        remoteUrl: 'https://example.test/assets/source-name.png?token=1',
        remoteMessageId: 'msg:/1',
        fileName: '../unsafe name.png',
        mimeType: 'image/png',
        fetchImpl: async () => ({
          ok: true,
          status: 200,
          headers: {
            get: (name: string) => (name.toLowerCase() === 'content-type' ? 'image/png' : null)
          },
          arrayBuffer: async () => new Uint8Array([1, 2, 3, 4]).buffer
        })
      })

      expect(result.localPath).toBe(
        join(rootDir, 'remote-im', 'images', 'project_1', 'msg_1.png')
      )
      expect(result.fileName).toBe('msg_1.png')
      expect(result.mimeType).toBe('image/png')
      expect(result.sizeBytes).toBe(4)
      await expect(readFile(result.localPath)).resolves.toEqual(Buffer.from([1, 2, 3, 4]))
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('reports HTTP download failures', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-image-cache-'))
    try {
      await expect(
        cacheRemoteImImage({
          rootDir,
          projectId: 'project-1',
          remoteUrl: 'https://example.test/not-found.jpg',
          remoteMessageId: 'msg-1',
          fetchImpl: async () => ({
            ok: false,
            status: 404,
            headers: { get: () => null },
            arrayBuffer: async () => new ArrayBuffer(0)
          })
        })
      ).rejects.toThrow('HTTP 404')
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })
})
