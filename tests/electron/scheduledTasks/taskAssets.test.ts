import { promises as fs } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { setActiveAccount } from '../../../electron/store/paths.js'
import {
  MAX_SCHEDULED_TASK_IMAGE_BYTES,
  saveScheduledTaskImage
} from '../../../electron/scheduledTasks/taskAssets.js'

let tempRoot: string | null = null

describe('scheduled task image assets', () => {
  beforeEach(async () => {
    tempRoot = await fs.mkdtemp(join(tmpdir(), 'scheduled-task-assets-'))
    process.env.MULTI_AI_ROOT = tempRoot
    setActiveAccount('test-account')
  })

  afterEach(async () => {
    setActiveAccount(null)
    delete process.env.MULTI_AI_ROOT
    if (tempRoot) await fs.rm(tempRoot, { recursive: true, force: true })
    tempRoot = null
  })

  it('stores a validated image in the account project directory', async () => {
    const png = Uint8Array.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])
    const attachment = await saveScheduledTaskImage({
      projectId: 'project-1',
      fileName: 'screen.png',
      mimeType: 'image/png',
      data: png
    })

    expect(attachment.fileName).toBe('screen.png')
    expect(attachment.mimeType).toBe('image/png')
    expect(attachment.localPath).toContain(
      join('accounts', 'test-account', 'projects', 'project-1', 'scheduled-task-images')
    )
    expect(await fs.readFile(attachment.localPath)).toEqual(Buffer.from(png))
  })

  it('rejects unsupported data and oversized images', async () => {
    await expect(
      saveScheduledTaskImage({
        projectId: 'project-1',
        fileName: 'fake.png',
        mimeType: 'image/png',
        data: Uint8Array.from([1, 2, 3])
      })
    ).rejects.toThrow('仅支持 PNG、JPEG、GIF 或 WebP 图片')

    const oversized = new Uint8Array(MAX_SCHEDULED_TASK_IMAGE_BYTES + 1)
    oversized.set([0x89, 0x50, 0x4e, 0x47])
    await expect(
      saveScheduledTaskImage({
        projectId: 'project-1',
        fileName: 'large.png',
        mimeType: 'image/png',
        data: oversized
      })
    ).rejects.toThrow('图片不能超过 20 MB')
  })

  it('rejects project IDs that could escape the project directory', async () => {
    const png = Uint8Array.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])
    await expect(
      saveScheduledTaskImage({
        projectId: '../outside',
        fileName: 'screen.png',
        mimeType: 'image/png',
        data: png
      })
    ).rejects.toThrow('项目 ID 无效')
  })
})
