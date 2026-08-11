import { mkdir, mkdtemp, rm, symlink, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it } from 'vitest'
import {
  loadRemoteImLocalImageForSend,
  readRemoteImLocalImageDataUrl
} from '../../../electron/remote-im/localImageFile.js'

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

  it('reads a persisted image as a renderer-safe data URL', async () => {
    const rootDir = await createTempDir()
    const imagePath = join(rootDir, 'history.png')
    const bytes = new Uint8Array([0x89, 0x50, 0x4e, 0x47])
    await writeFile(imagePath, bytes)

    const dataUrl = await readRemoteImLocalImageDataUrl(imagePath, {
      maxBytes: 1024,
      allowedDirectory: rootDir
    })

    expect(dataUrl).toBe(`data:image/png;base64,${Buffer.from(bytes).toString('base64')}`)
  })

  it('does not read an oversized persisted image preview', async () => {
    const rootDir = await createTempDir()
    const imagePath = join(rootDir, 'large.png')
    await writeFile(imagePath, new Uint8Array([1, 2, 3]))

    await expect(
      readRemoteImLocalImageDataUrl(imagePath, { maxBytes: 2, allowedDirectory: rootDir })
    ).rejects.toThrow('图片文件过大')
  })

  it('does not read an image outside the controlled cache directory', async () => {
    const rootDir = await createTempDir()
    const cacheDir = join(rootDir, 'cache')
    const imagePath = join(rootDir, 'private.png')
    await writeFile(imagePath, new Uint8Array([1, 2, 3]))

    await expect(
      readRemoteImLocalImageDataUrl(imagePath, {
        maxBytes: 1024,
        allowedDirectory: cacheDir
      })
    ).rejects.toThrow('图片不属于当前项目缓存')
  })

  it('does not follow a cache symlink to an image outside the controlled directory', async () => {
    const rootDir = await createTempDir()
    const cacheDir = join(rootDir, 'cache')
    const privateImage = join(rootDir, 'private.png')
    const linkedImage = join(cacheDir, 'linked.png')
    await mkdir(cacheDir)
    await writeFile(privateImage, new Uint8Array([1, 2, 3]))
    await symlink(privateImage, linkedImage)

    await expect(
      readRemoteImLocalImageDataUrl(linkedImage, {
        maxBytes: 1024,
        allowedDirectory: cacheDir
      })
    ).rejects.toThrow('图片不属于当前项目缓存')
  })
})
