import { promises as fs } from 'node:fs'
import { isAbsolute, relative, resolve, sep } from 'node:path'
import type { RemoteImImageAttachment } from './types.js'

const IMAGE_EXTENSIONS = new Set(['png', 'jpg', 'jpeg', 'gif', 'webp'])

const IMAGE_MIME_BY_EXTENSION: Record<string, string> = {
  gif: 'image/gif',
  jpeg: 'image/jpeg',
  jpg: 'image/jpeg',
  png: 'image/png',
  webp: 'image/webp'
}

export interface RemoteImLocalImagePayload {
  attachment: RemoteImImageAttachment
  fileName: string
  mimeType: string
  fileBytes: ArrayBuffer
}

export interface LoadRemoteImLocalImageOptions {
  maxBytes: number
}

export interface ReadRemoteImLocalImageOptions {
  maxBytes: number
  allowedDirectory: string
}

function toTransferableArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new ArrayBuffer(bytes.byteLength)
  new Uint8Array(copy).set(bytes)
  return copy
}

function imageExtension(path: string): string | null {
  const match = /\.([A-Za-z0-9]+)$/.exec(path.trim())
  if (!match) return null
  const ext = match[1].toLowerCase()
  return IMAGE_EXTENSIONS.has(ext) ? ext : null
}

function mimeTypeFromRemoteImImagePath(path: string): string | null {
  const ext = imageExtension(path)
  return ext ? IMAGE_MIME_BY_EXTENSION[ext] ?? null : null
}

function fileNameFromPath(path: string): string | null {
  const fileName = path.split(/[\\/]/).filter(Boolean).pop()?.trim()
  return fileName || null
}

function createRemoteImImageAttachmentFromLocalPath(
  localPath: string,
  sizeBytes: number | null = null
): RemoteImImageAttachment {
  return {
    type: 'image',
    localPath,
    remoteUrl: null,
    thumbnailUrl: null,
    width: null,
    height: null,
    sizeBytes,
    fileName: fileNameFromPath(localPath),
    mimeType: mimeTypeFromRemoteImImagePath(localPath),
    sdkImageId: null
  }
}

export async function loadRemoteImLocalImageForSend(
  localPath: string,
  options: LoadRemoteImLocalImageOptions
): Promise<RemoteImLocalImagePayload> {
  const cleanPath = localPath.trim()
  if (!cleanPath) throw new Error('image path is required')

  const mimeType = mimeTypeFromRemoteImImagePath(cleanPath)
  if (!mimeType) throw new Error('unsupported image type')

  const stat = await fs.stat(cleanPath)
  if (!stat.isFile()) throw new Error('image path is not a file')
  if (stat.size > options.maxBytes) throw new Error('image file is too large')

  const attachment = createRemoteImImageAttachmentFromLocalPath(cleanPath, stat.size)
  const bytes = await fs.readFile(cleanPath)
  return {
    attachment,
    fileName: attachment.fileName ?? 'remote-im-image.png',
    mimeType,
    fileBytes: toTransferableArrayBuffer(bytes)
  }
}

/**
 * Read an image that has already been associated with a persisted IM message.
 * Callers must resolve the path from the trusted message record instead of
 * accepting an arbitrary renderer-supplied path.
 */
export async function readRemoteImLocalImageDataUrl(
  localPath: string,
  options: ReadRemoteImLocalImageOptions
): Promise<string> {
  const cleanPath = localPath.trim()
  if (!cleanPath) throw new Error('图片本地路径为空')
  if (!isAbsolute(cleanPath)) throw new Error('图片本地路径无效')

  const allowedDirectory = resolve(options.allowedDirectory)
  const requestedPath = resolve(cleanPath)
  const isInside = (directory: string, candidate: string): boolean => {
    const pathFromDirectory = relative(directory, candidate)
    return (
      pathFromDirectory.length > 0 &&
      pathFromDirectory !== '..' &&
      !pathFromDirectory.startsWith(`..${sep}`) &&
      !isAbsolute(pathFromDirectory)
    )
  }
  if (!isInside(allowedDirectory, requestedPath)) {
    throw new Error('图片不属于当前项目缓存')
  }

  const [realDirectory, realPath] = await Promise.all([
    fs.realpath(allowedDirectory),
    fs.realpath(requestedPath)
  ])
  if (!isInside(realDirectory, realPath)) {
    throw new Error('图片不属于当前项目缓存')
  }

  const mimeType = mimeTypeFromRemoteImImagePath(realPath)
  if (!mimeType) throw new Error('图片格式不支持')

  const stat = await fs.stat(realPath)
  if (!stat.isFile()) throw new Error('图片文件不存在')
  if (stat.size === 0) throw new Error('图片内容为空')
  if (stat.size > options.maxBytes) throw new Error('图片文件过大')

  const bytes = await fs.readFile(realPath)
  return `data:${mimeType};base64,${bytes.toString('base64')}`
}
