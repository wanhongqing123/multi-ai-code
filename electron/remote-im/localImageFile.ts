import { promises as fs } from 'node:fs'
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
