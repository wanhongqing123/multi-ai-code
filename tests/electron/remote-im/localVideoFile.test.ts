import { mkdtemp, rm, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it } from 'vitest'
import {
  loadRemoteImLocalVideoForSend,
  mimeTypeFromRemoteImVideoPath
} from '../../../electron/remote-im/localVideoFile.js'

let tempDir: string | null = null

async function createTempDir(): Promise<string> {
  tempDir = await mkdtemp(join(tmpdir(), 'remote-im-local-video-'))
  return tempDir
}

describe('remote IM local video loading', () => {
  afterEach(async () => {
    if (tempDir) await rm(tempDir, { recursive: true, force: true })
    tempDir = null
  })

  it('loads an mp4 into attachment metadata and IPC bytes', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'screen-record.mp4')
    await writeFile(filePath, new Uint8Array([0, 0, 0, 24, 0x66, 0x74, 0x79, 0x70]))

    const result = await loadRemoteImLocalVideoForSend(filePath, { maxBytes: 1024 })

    expect(result.attachment).toMatchObject({
      type: 'video',
      localPath: filePath,
      fileName: 'screen-record.mp4',
      mimeType: 'video/mp4',
      sizeBytes: 8,
      // 封面/宽高/时长要等服务端生成，出站阶段本端一个都拿不到。
      thumbnailUrl: null,
      width: null,
      height: null,
      durationSeconds: null
    })
    expect(new Uint8Array(result.fileBytes)).toHaveLength(8)
    expect(result.fileName).toBe('screen-record.mp4')
    expect(result.mimeType).toBe('video/mp4')
  })

  it('maps mov to video/quicktime because that is the subtype the SDK whitelists', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'clip.MOV')
    await writeFile(filePath, new Uint8Array([1, 2, 3, 4]))

    const result = await loadRemoteImLocalVideoForSend(filePath, { maxBytes: 1024 })

    expect(result.mimeType).toBe('video/quicktime')
    expect(result.attachment.fileName).toBe('clip.MOV')
  })

  it('rejects containers the IM SDK cannot accept before reading the file', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'clip.mkv')
    await writeFile(filePath, new Uint8Array([1, 2, 3]))

    await expect(loadRemoteImLocalVideoForSend(filePath, { maxBytes: 1024 })).rejects.toThrow(
      'unsupported video type'
    )
    expect(mimeTypeFromRemoteImVideoPath(filePath)).toBeNull()
  })

  it('rejects videos above the size limit', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'big.mp4')
    await writeFile(filePath, new Uint8Array(64))

    await expect(loadRemoteImLocalVideoForSend(filePath, { maxBytes: 16 })).rejects.toThrow(
      'video file is too large'
    )
  })

  it('rejects an empty video instead of uploading a zero byte file', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'empty.mp4')
    await writeFile(filePath, new Uint8Array(0))

    await expect(loadRemoteImLocalVideoForSend(filePath, { maxBytes: 1024 })).rejects.toThrow(
      'video file is empty'
    )
  })
})
