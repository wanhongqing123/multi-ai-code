import { promises as fs } from 'node:fs'
import {
  createRemoteImImageAttachmentFromLocalPath,
  mimeTypeFromRemoteImImagePath
} from './aicliImageOutput.js'
import type { RemoteImImageAttachment } from './types.js'

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
