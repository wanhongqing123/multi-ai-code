import { mkdtemp, readFile, rm } from 'fs/promises'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it } from 'vitest'
import { cacheRemoteImFile } from '../../../electron/remote-im/fileCache.js'

function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  // 不能直接用 bytes.buffer：它的类型是 ArrayBufferLike（可能是 SharedArrayBuffer）。
  const copy = new ArrayBuffer(bytes.byteLength)
  new Uint8Array(copy).set(bytes)
  return copy
}

function okResponse(bytes: Uint8Array, contentType: string | null) {
  return {
    ok: true,
    status: 200,
    headers: { get: (name: string) => (name.toLowerCase() === 'content-type' ? contentType : null) },
    arrayBuffer: async () => toArrayBuffer(bytes)
  }
}

describe('remote IM file cache', () => {
  it('accepts arbitrary file types, not just markdown/html', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-file-cache-'))
    try {
      // 以前这里按 MIME 白名单只放行 text/markdown 与 text/html，其余一律抛
      // 'unsupported file type'。用户发来的 pdf/zip/docx 因此收不到，而发送侧
      // （send-file）早就不限类型——接收侧必须对称。
      const result = await cacheRemoteImFile({
        rootDir,
        projectId: 'project:/1',
        remoteUrl: 'https://example.test/assets/spec.pdf?token=1',
        remoteMessageId: 'msg:/1',
        fileName: '../unsafe spec.pdf',
        mimeType: 'application/pdf',
        fetchImpl: async () => okResponse(new Uint8Array([1, 2, 3, 4]), 'application/pdf')
      })

      expect(result.localPath).toBe(join(rootDir, 'remote-im', 'files', 'project_1', 'msg_1.pdf'))
      expect(result.mimeType).toBe('application/pdf')
      expect(result.sizeBytes).toBe(4)
      await expect(readFile(result.localPath)).resolves.toEqual(Buffer.from([1, 2, 3, 4]))
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('keeps the original extension even for types it has no MIME mapping for', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-file-cache-'))
    try {
      // 旧实现只保留"能映射回已知 MIME"的扩展名，.dwg 这类会被丢掉并统一落成
      // .md —— 用户拿到手的文件双击打不开。
      const result = await cacheRemoteImFile({
        rootDir,
        projectId: 'p1',
        remoteUrl: 'https://example.test/drawing.dwg',
        remoteMessageId: 'msg-dwg',
        fileName: 'drawing.dwg',
        mimeType: null,
        fetchImpl: async () => okResponse(new Uint8Array([9]), null)
      })

      expect(result.localPath.endsWith('.dwg')).toBe(true)
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('falls back to .bin rather than mislabelling unknown binaries as markdown', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-file-cache-'))
    try {
      const result = await cacheRemoteImFile({
        rootDir,
        projectId: 'p1',
        remoteUrl: 'https://example.test/download',
        remoteMessageId: 'msg-blob',
        fileName: null,
        mimeType: null,
        fetchImpl: async () => okResponse(new Uint8Array([0, 1, 2]), null)
      })

      expect(result.localPath.endsWith('.bin')).toBe(true)
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('prefers the declared MIME over a generic content-type from object storage', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-file-cache-'))
    try {
      // 对象存储对未知类型常年回 application/octet-stream，用它覆盖发送方声明
      // 的类型只会把信息变少。
      const result = await cacheRemoteImFile({
        rootDir,
        projectId: 'p1',
        remoteUrl: 'https://example.test/report.md',
        remoteMessageId: 'msg-md',
        fileName: 'report.md',
        mimeType: 'text/markdown',
        fetchImpl: async () => okResponse(new Uint8Array([1]), 'application/octet-stream')
      })

      expect(result.mimeType).toBe('text/markdown')
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('still enforces the size cap', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-file-cache-'))
    try {
      // 放开类型限制后，体积上限就是唯一的护栏，不能跟着一起丢。
      await expect(
        cacheRemoteImFile({
          rootDir,
          projectId: 'p1',
          remoteUrl: 'https://example.test/big.zip',
          remoteMessageId: 'msg-big',
          fileName: 'big.zip',
          mimeType: 'application/zip',
          maxBytes: 2,
          fetchImpl: async () => okResponse(new Uint8Array([1, 2, 3, 4]), 'application/zip')
        })
      ).rejects.toThrow('file is too large')
    } finally {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it('reports HTTP download failures', async () => {
    const rootDir = await mkdtemp(join(tmpdir(), 'remote-im-file-cache-'))
    try {
      await expect(
        cacheRemoteImFile({
          rootDir,
          projectId: 'p1',
          remoteUrl: 'https://example.test/missing.pdf',
          remoteMessageId: 'msg-404',
          fileName: 'missing.pdf',
          mimeType: 'application/pdf',
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
