import { randomUUID } from 'node:crypto'
import { promises as fs } from 'node:fs'
import { isAbsolute, join, relative, resolve, sep } from 'node:path'
import { projectDir } from '../store/paths.js'
import type {
  ReadScheduledTaskImageInput,
  SaveScheduledTaskImageInput,
  ScheduledTaskImageAttachment
} from './types.js'

export const MAX_SCHEDULED_TASK_IMAGE_BYTES = 20 * 1024 * 1024

const IMAGE_TYPES = {
  gif: { extension: 'gif', mimeType: 'image/gif' },
  jpeg: { extension: 'jpg', mimeType: 'image/jpeg' },
  png: { extension: 'png', mimeType: 'image/png' },
  webp: { extension: 'webp', mimeType: 'image/webp' }
} as const

type ImageType = keyof typeof IMAGE_TYPES

function toBuffer(data: ArrayBuffer | Uint8Array): Buffer {
  if (data instanceof ArrayBuffer) return Buffer.from(data)
  return Buffer.from(data.buffer, data.byteOffset, data.byteLength)
}

function detectImageType(bytes: Buffer): ImageType | null {
  if (
    bytes.length >= 8 &&
    bytes[0] === 0x89 &&
    bytes.subarray(1, 4).toString('ascii') === 'PNG'
  ) {
    return 'png'
  }
  if (bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) {
    return 'jpeg'
  }
  if (bytes.length >= 6) {
    const header = bytes.subarray(0, 6).toString('ascii')
    if (header === 'GIF87a' || header === 'GIF89a') return 'gif'
  }
  if (
    bytes.length >= 12 &&
    bytes.subarray(0, 4).toString('ascii') === 'RIFF' &&
    bytes.subarray(8, 12).toString('ascii') === 'WEBP'
  ) {
    return 'webp'
  }
  return null
}

function displayFileName(fileName: string, fallbackExtension: string): string {
  const name = fileName.split(/[\\/]/).pop()?.trim().replace(/[\u0000-\u001f]/g, '')
  return name || `task-image.${fallbackExtension}`
}

function validateProjectId(projectId: string): string {
  const normalized = projectId.trim()
  if (!/^[A-Za-z0-9_-]{1,128}$/.test(normalized)) {
    throw new Error('项目 ID 无效')
  }
  return normalized
}

function isPathInside(directory: string, candidate: string): boolean {
  const pathFromDirectory = relative(directory, candidate)
  return (
    pathFromDirectory.length > 0 &&
    pathFromDirectory !== '..' &&
    !pathFromDirectory.startsWith(`..${sep}`) &&
    !isAbsolute(pathFromDirectory)
  )
}

export async function saveScheduledTaskImage(
  input: SaveScheduledTaskImageInput
): Promise<ScheduledTaskImageAttachment> {
  const projectId = validateProjectId(input.projectId)

  const bytes = toBuffer(input.data)
  if (bytes.length === 0) throw new Error('图片内容为空')
  if (bytes.length > MAX_SCHEDULED_TASK_IMAGE_BYTES) {
    throw new Error('图片不能超过 20 MB')
  }

  const imageType = detectImageType(bytes)
  if (!imageType) throw new Error('仅支持 PNG、JPEG、GIF 或 WebP 图片')

  const imageInfo = IMAGE_TYPES[imageType]
  const id = randomUUID()
  const directory = join(projectDir(projectId), 'scheduled-task-images')
  const localPath = join(directory, `${id}.${imageInfo.extension}`)
  await fs.mkdir(directory, { recursive: true })
  await fs.writeFile(localPath, bytes)

  return {
    id,
    localPath,
    fileName: displayFileName(input.fileName, imageInfo.extension),
    mimeType: imageInfo.mimeType,
    sizeBytes: bytes.length
  }
}

export async function readScheduledTaskImage(
  input: ReadScheduledTaskImageInput
): Promise<string> {
  const projectId = validateProjectId(input.projectId)
  if (!isAbsolute(input.localPath)) {
    throw new Error('任务图片路径无效')
  }

  const imageDirectory = resolve(projectDir(projectId), 'scheduled-task-images')
  const requestedPath = resolve(input.localPath)
  if (!isPathInside(imageDirectory, requestedPath)) {
    throw new Error('任务图片不属于当前项目')
  }

  const [realDirectory, realPath] = await Promise.all([
    fs.realpath(imageDirectory),
    fs.realpath(requestedPath)
  ])
  if (!isPathInside(realDirectory, realPath)) {
    throw new Error('任务图片不属于当前项目')
  }

  const stat = await fs.stat(realPath)
  if (!stat.isFile()) throw new Error('任务图片不存在')
  if (stat.size === 0) throw new Error('图片内容为空')
  if (stat.size > MAX_SCHEDULED_TASK_IMAGE_BYTES) {
    throw new Error('图片不能超过 20 MB')
  }

  const bytes = await fs.readFile(realPath)
  const imageType = detectImageType(bytes)
  if (!imageType) throw new Error('任务图片格式无效')
  return `data:${IMAGE_TYPES[imageType].mimeType};base64,${bytes.toString('base64')}`
}
