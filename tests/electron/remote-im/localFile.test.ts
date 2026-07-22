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

  it('rejects unsupported document extensions before reading the file', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'report.pdf')
    await writeFile(filePath, new Uint8Array([1, 2, 3]))

    await expect(loadRemoteImLocalFileForSend(filePath, { maxBytes: 1024 })).rejects.toThrow(
      'unsupported file type'
    )
  })
})
