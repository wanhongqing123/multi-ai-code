import { promises as fs } from 'node:fs'
import type { RemoteImVideoAttachment } from './types.js'

/**
 * 腾讯 IM Web SDK 只认这几种视频容器（内部按 MIME 子类型白名单校验，
 * 见 lite-chat 的 _validateBeforeUploadVideo：["mp4","quicktime","mov","video"]）。
 * 别的扩展名一律拒在本端，免得整包传上去才被 SDK 以 2352 打回。
 */
const VIDEO_EXTENSIONS = new Set(['mp4', 'mov'])

const VIDEO_MIME_BY_EXTENSION: Record<string, string> = {
  mov: 'video/quicktime',
  mp4: 'video/mp4'
}

export interface RemoteImLocalVideoPayload {
  attachment: RemoteImVideoAttachment
  fileName: string
  mimeType: string
  fileBytes: ArrayBuffer
}

export interface LoadRemoteImLocalVideoOptions {
  maxBytes: number
}

function toTransferableArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new ArrayBuffer(bytes.byteLength)
  new Uint8Array(copy).set(bytes)
  return copy
}

function videoExtension(path: string): string | null {
  const match = /\.([A-Za-z0-9]+)$/.exec(path.trim())
  if (!match) return null
  const ext = match[1].toLowerCase()
  return VIDEO_EXTENSIONS.has(ext) ? ext : null
}

export function mimeTypeFromRemoteImVideoPath(path: string): string | null {
  const ext = videoExtension(path)
  return ext ? VIDEO_MIME_BY_EXTENSION[ext] ?? null : null
}

function fileNameFromPath(path: string): string | null {
  const fileName = path.split(/[\\/]/).filter(Boolean).pop()?.trim()
  return fileName || null
}

function createRemoteImVideoAttachmentFromLocalPath(
  localPath: string,
  sizeBytes: number | null = null
): RemoteImVideoAttachment {
  return {
    type: 'video',
    localPath,
    remoteUrl: null,
    thumbnailUrl: null,
    width: null,
    height: null,
    durationSeconds: null,
    sizeBytes,
    fileName: fileNameFromPath(localPath),
    mimeType: mimeTypeFromRemoteImVideoPath(localPath),
    sdkVideoId: null
  }
}

export async function loadRemoteImLocalVideoForSend(
  localPath: string,
  options: LoadRemoteImLocalVideoOptions
): Promise<RemoteImLocalVideoPayload> {
  const cleanPath = localPath.trim()
  if (!cleanPath) throw new Error('video path is required')

  const mimeType = mimeTypeFromRemoteImVideoPath(cleanPath)
  if (!mimeType) throw new Error('unsupported video type')

  const stat = await fs.stat(cleanPath)
  if (!stat.isFile()) throw new Error('video path is not a file')
  if (stat.size === 0) throw new Error('video file is empty')
  if (stat.size > options.maxBytes) throw new Error('video file is too large')

  const attachment = createRemoteImVideoAttachmentFromLocalPath(cleanPath, stat.size)
  const bytes = await fs.readFile(cleanPath)
  return {
    attachment,
    fileName: attachment.fileName ?? 'remote-im-video.mp4',
    mimeType,
    fileBytes: toTransferableArrayBuffer(bytes)
  }
}
