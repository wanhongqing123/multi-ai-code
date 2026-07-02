import { mkdtemp, rm, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it } from 'vitest'
import { loadRemoteImLocalImageForSend } from './localImageFile.js'

let tempDir: string | null = null

async function createTempDir(): Promise<string> {
  tempDir = await mkdtemp(join(tmpdir(), 'remote-im-local-image-'))
  return tempDir
}

describe('remote IM local image file loading', () => {
  afterEach(async () => {
    if (tempDir) await rm(tempDir, { recursive: true, force: true })
    tempDir = null
  })

  it('loads a supported local image into attachment metadata and IPC bytes', async () => {
    const rootDir = await createTempDir()
    const imagePath = join(rootDir, 'photo.png')
    await writeFile(imagePath, new Uint8Array([1, 2, 3]))

    const result = await loadRemoteImLocalImageForSend(imagePath, { maxBytes: 1024 })

    expect(result.attachment).toMatchObject({
      type: 'image',
      localPath: imagePath,
      fileName: 'photo.png',
      mimeType: 'image/png',
      sizeBytes: 3
    })
    expect(new Uint8Array(result.fileBytes)).toEqual(new Uint8Array([1, 2, 3]))
    expect(result.fileName).toBe('photo.png')
    expect(result.mimeType).toBe('image/png')
  })

  it('rejects unsupported image extensions before reading the file', async () => {
    const rootDir = await createTempDir()
    const imagePath = join(rootDir, 'photo.bmp')
    await writeFile(imagePath, new Uint8Array([1, 2, 3]))

    await expect(loadRemoteImLocalImageForSend(imagePath, { maxBytes: 1024 })).rejects.toThrow(
      'unsupported image type'
    )
  })
})
