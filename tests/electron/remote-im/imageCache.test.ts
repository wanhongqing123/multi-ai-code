import { mkdtemp, readFile, rm } from 'fs/promises'
import { basename, dirname, join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it, vi } from 'vitest'
import {
  cacheRemoteImImage,
  cacheRemoteImImageBytes,
  remoteImImageCacheDirectory
} from '../../../electron/remote-im/imageCache.js'

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

  it.each(['request', 'body'] as const)('recovers a transient %s failure before caching the image', async (stage) => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-image-network-'))
    vi.useFakeTimers()
    const fetchImpl = vi.fn(async () => {
      if (fetchImpl.mock.calls.length === 1 && stage === 'request') throw new Error('net::ERR_FAILED')
      const firstBodyFails = fetchImpl.mock.calls.length === 1 && stage === 'body'
      return {
        ok: true, status: 200,
        arrayBuffer: async () => {
          if (firstBodyFails) throw new TypeError('terminated', { cause: { code: 'UND_ERR_SOCKET' } })
          return new Uint8Array([1, 2, 3, 4]).buffer
        }
      }
    })
    try {
      const pending = cacheRemoteImImage({
        rootDir, projectId: 'p1', remoteUrl: 'https://example.test/image.png',
        remoteMessageId: 'image-1', fetchImpl
      })
      await vi.advanceTimersByTimeAsync(250)
      const result = await pending
      expect(fetchImpl).toHaveBeenCalledTimes(2)
      await expect(readFile(result.localPath)).resolves.toEqual(Buffer.from([1, 2, 3, 4]))
    } finally {
      vi.useRealTimers()
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('bounds retries when the network remains unavailable', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-image-offline-'))
    vi.useFakeTimers()
    const fetchImpl = vi.fn(async () => { throw new Error('net::ERR_FAILED') })
    try {
      const pending = expect(cacheRemoteImImage({
        rootDir, projectId: 'p1', remoteUrl: 'https://example.test/image.png', fetchImpl
      })).rejects.toThrow('net::ERR_FAILED')
      await vi.advanceTimersByTimeAsync(750)
      await pending
      expect(fetchImpl).toHaveBeenCalledTimes(3)
    } finally {
      vi.useRealTimers()
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it.each([
    { error: new Error('HTTP 403'), status: 403 },
    { error: new TypeError('Failed to parse URL'), status: null },
    { error: new DOMException('Canceled', 'AbortError'), status: null }
  ])('does not retry HTTP denials, invalid URLs, or cancellation: $error.message', async ({ error, status }) => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-image-nonretryable-'))
    const fetchImpl = vi.fn(async () => {
      if (status === null) throw error
      return { ok: false, status, arrayBuffer: async () => new ArrayBuffer(0) }
    })
    try {
      await expect(cacheRemoteImImage({
        rootDir, projectId: 'p1', remoteUrl: 'https://example.test/image.png', fetchImpl
      })).rejects.toThrow(error.message)
      expect(fetchImpl).toHaveBeenCalledOnce()
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('persists an outgoing image inside the controlled project cache', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-image-cache-'))
    try {
      const bytes = new Uint8Array([1, 2, 3, 4])
      const result = await cacheRemoteImImageBytes({
        rootDir,
        projectId: 'project:/1',
        fileName: 'screen.png',
        mimeType: 'image/png',
        bytes
      })

      // Compare the directory itself so Windows separators cannot weaken the cache-boundary check.
      expect(dirname(result.localPath)).toBe(remoteImImageCacheDirectory(rootDir, 'project:/1'))
      expect(basename(result.localPath)).toMatch(/^outgoing-[0-9a-f-]+\.png$/)
      expect(result.fileName).toBe('screen.png')
      await expect(readFile(result.localPath)).resolves.toEqual(Buffer.from(bytes))
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })
})
