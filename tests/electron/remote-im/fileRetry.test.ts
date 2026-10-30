import { mkdtemp, readFile, rm } from 'fs/promises'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it, vi } from 'vitest'
import { retryRemoteImFile } from '../../../electron/remote-im/fileRetry.js'
import type { RemoteImFileAttachment, RemoteImMessage } from '../../../electron/remote-im/types.js'

describe('remote IM file retry', () => {
  it('restores only the original incoming file and never dispatches it to an Agent', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'im-file-retry-'))
    let message = {
      id: 7, projectId: 'p1', direction: 'incoming', status: 'failed', kind: 'file',
      remoteMessageId: 'remote-7',
      attachment: { type: 'file', remoteUrl: 'https://example.test/doc.md', localPath: null }
    } as RemoteImMessage
    const fetchImpl = vi.fn(async () => ({
      ok: true, status: 200, arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer
    }))
    const restore = vi.fn((projectId: string, id: number, url: string, attachment: RemoteImFileAttachment) => {
      if (message.projectId !== projectId || message.id !== id || message.status !== 'failed' ||
          message.attachment?.type !== 'file' || message.attachment.remoteUrl !== url) return null
      message = { ...message, attachment, status: 'received', error: null }
      return message
    })
    const deps = { rootDir, fetchImpl, maxBytes: 20 * 1024 * 1024,
      listById: () => message, restore, isAccountCurrent: () => true }
    try {
      await expect(retryRemoteImFile('other', 7, deps)).rejects.toThrow()
      expect(fetchImpl).not.toHaveBeenCalled()
      await expect(retryRemoteImFile('p1', 7, { ...deps, isAccountCurrent: () => false }))
        .rejects.toThrow('account changed')
      expect(restore).not.toHaveBeenCalled()
      await retryRemoteImFile('p1', 7, deps)
      expect(restore).toHaveBeenCalledOnce()
      expect(message.status).toBe('received')
      expect(message.attachment?.type).toBe('file')
      if (message.attachment?.type !== 'file' || !message.attachment.localPath) throw new Error('missing file')
      await expect(readFile(message.attachment.localPath)).resolves.toEqual(Buffer.from([1, 2, 3]))
      // 重复点击已经恢复的记录不会再下载，也不会创建或投递第二条消息。
      await expect(retryRemoteImFile('p1', 7, deps)).rejects.toThrow('file cannot be retried')
      expect(fetchImpl).toHaveBeenCalledTimes(2)
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('leaves the failed record unchanged if the signed URL has expired', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'im-file-expired-'))
    const message = {
      id: 8, projectId: 'p1', direction: 'incoming', status: 'failed', kind: 'file',
      attachment: { type: 'file', remoteUrl: 'https://example.test/expired', localPath: null }
    } as RemoteImMessage
    const restore = vi.fn()
    try {
      await expect(retryRemoteImFile('p1', 8, {
        rootDir, maxBytes: 20 * 1024 * 1024,
        listById: () => message, restore, isAccountCurrent: () => true,
        fetchImpl: async () => ({ ok: false, status: 403, arrayBuffer: async () => new ArrayBuffer(0) })
      })).rejects.toThrow('HTTP 403')
      expect(restore).not.toHaveBeenCalled()
      expect(message.status).toBe('failed')
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('uses the caller limit for a file larger than the cache default', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'im-file-large-'))
    const message = {
      id: 9, projectId: 'p1', direction: 'incoming', status: 'failed', kind: 'file',
      attachment: { type: 'file', remoteUrl: 'https://example.test/large', localPath: null }
    } as RemoteImMessage
    const restore = vi.fn(() => message)
    const body = new Uint8Array(5 * 1024 * 1024 + 1).buffer
    const deps = {
      rootDir, listById: () => message, restore, isAccountCurrent: () => true,
      fetchImpl: async () => ({ ok: true, status: 200, arrayBuffer: async () => body })
    }
    try {
      await expect(retryRemoteImFile('p1', 9, { ...deps, maxBytes: body.byteLength - 1 }))
        .rejects.toThrow('file is too large')
      expect(restore).not.toHaveBeenCalled()
      await retryRemoteImFile('p1', 9, { ...deps, maxBytes: body.byteLength })
      expect(restore).toHaveBeenCalledOnce()
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })
})
