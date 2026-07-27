import { mkdtemp, rm, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it } from 'vitest'
import { loadRemoteImLocalFileForSend } from '../../../electron/remote-im/localFile.js'

let tempDir: string | null = null

async function createTempDir(): Promise<string> {
  tempDir = await mkdtemp(join(tmpdir(), 'remote-im-local-file-'))
  return tempDir
}

describe('remote IM local document file loading', () => {
  afterEach(async () => {
    if (tempDir) await rm(tempDir, { recursive: true, force: true })
    tempDir = null
  })

  it('loads a supported markdown file into attachment metadata and IPC bytes', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'report.md')
    await writeFile(filePath, '# Report\n\n- passed\n')

    const result = await loadRemoteImLocalFileForSend(filePath, { maxBytes: 1024 })

    expect(result.attachment).toMatchObject({
      type: 'file',
      localPath: filePath,
      fileName: 'report.md',
      mimeType: 'text/markdown',
      sizeBytes: 19
    })
    expect(new TextDecoder().decode(result.fileBytes)).toBe('# Report\n\n- passed\n')
    expect(result.fileName).toBe('report.md')
    expect(result.mimeType).toBe('text/markdown')
  })

  it('loads a supported html file into attachment metadata and IPC bytes', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'report.html')
    await writeFile(filePath, '<h1>Report</h1>')

    const result = await loadRemoteImLocalFileForSend(filePath, { maxBytes: 1024 })

    expect(result.attachment).toMatchObject({
      type: 'file',
      localPath: filePath,
      fileName: 'report.html',
      mimeType: 'text/html',
      sizeBytes: 15
    })
  })

  it('loads a generic file with a known mime type', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'bundle.zip')
    await writeFile(filePath, new Uint8Array([0x50, 0x4b, 3, 4]))

    const result = await loadRemoteImLocalFileForSend(filePath, { maxBytes: 1024 })

    expect(result.attachment).toMatchObject({
      type: 'file',
      fileName: 'bundle.zip',
      mimeType: 'application/zip',
      sizeBytes: 4
    })
    expect(result.mimeType).toBe('application/zip')
  })

  it('loads an unknown-extension file as application/octet-stream', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'firmware.xyzdata')
    await writeFile(filePath, new Uint8Array([1, 2, 3]))

    const result = await loadRemoteImLocalFileForSend(filePath, { maxBytes: 1024 })

    expect(result.mimeType).toBe('application/octet-stream')
    expect(result.attachment.fileName).toBe('firmware.xyzdata')
  })

  it('still rejects files above the size limit', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'big.bin')
    await writeFile(filePath, new Uint8Array(64))

    await expect(loadRemoteImLocalFileForSend(filePath, { maxBytes: 16 })).rejects.toThrow(
      'file is too large'
    )
  })
})
