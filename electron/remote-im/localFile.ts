import { promises as fs } from 'node:fs'
import type { RemoteImFileAttachment } from './types.js'

const DOCUMENT_EXTENSIONS = new Set(['md', 'markdown', 'html', 'htm'])

const DOCUMENT_MIME_BY_EXTENSION: Record<string, string> = {
  htm: 'text/html',
  html: 'text/html',
  markdown: 'text/markdown',
  md: 'text/markdown'
}

// 普通文件的常见 MIME；未知扩展名回退 application/octet-stream。
// send-file 不再限制文件类型：md/html 走接收端预览，其余显示文件卡片供保存。
const GENERIC_MIME_BY_EXTENSION: Record<string, string> = {
  '7z': 'application/x-7z-compressed',
  csv: 'text/csv',
  gif: 'image/gif',
  gz: 'application/gzip',
  jpeg: 'image/jpeg',
  jpg: 'image/jpeg',
  json: 'application/json',
  log: 'text/plain',
  mp3: 'audio/mpeg',
  mp4: 'video/mp4',
  pdf: 'application/pdf',
  png: 'image/png',
  txt: 'text/plain',
  webp: 'image/webp',
  xml: 'application/xml',
  zip: 'application/zip'
}

export interface RemoteImLocalFilePayload {
  attachment: RemoteImFileAttachment
  fileName: string
  mimeType: string
  fileBytes: ArrayBuffer
}

export interface LoadRemoteImLocalFileOptions {
  maxBytes: number
}

function toTransferableArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new ArrayBuffer(bytes.byteLength)
  new Uint8Array(copy).set(bytes)
  return copy
}

function fileExtension(path: string): string | null {
  const match = /\.([A-Za-z0-9]+)$/.exec(path.trim())
  return match ? match[1].toLowerCase() : null
}

export function mimeTypeFromRemoteImFilePath(path: string): string {
  const ext = fileExtension(path)
  if (!ext) return 'application/octet-stream'
  if (DOCUMENT_EXTENSIONS.has(ext)) return DOCUMENT_MIME_BY_EXTENSION[ext]
  return GENERIC_MIME_BY_EXTENSION[ext] ?? 'application/octet-stream'
}

function fileNameFromPath(path: string): string | null {
  const fileName = path.split(/[\\/]/).filter(Boolean).pop()?.trim()
  return fileName || null
}

function createRemoteImFileAttachmentFromLocalPath(
  localPath: string,
  sizeBytes: number | null = null
): RemoteImFileAttachment {
  return {
    type: 'file',
    localPath,
    remoteUrl: null,
    sizeBytes,
    fileName: fileNameFromPath(localPath),
    mimeType: mimeTypeFromRemoteImFilePath(localPath),
    sdkFileId: null
  }
}

export async function loadRemoteImLocalFileForSend(
  localPath: string,
  options: LoadRemoteImLocalFileOptions
): Promise<RemoteImLocalFilePayload> {
  const cleanPath = localPath.trim()
  if (!cleanPath) throw new Error('file path is required')

  const mimeType = mimeTypeFromRemoteImFilePath(cleanPath)

  const stat = await fs.stat(cleanPath)
  if (!stat.isFile()) throw new Error('file path is not a file')
  if (stat.size > options.maxBytes) throw new Error('file is too large')

  const attachment = createRemoteImFileAttachmentFromLocalPath(cleanPath, stat.size)
  const bytes = await fs.readFile(cleanPath)
  return {
    attachment,
    fileName: attachment.fileName ?? 'remote-im-file',
    mimeType,
    fileBytes: toTransferableArrayBuffer(bytes)
  }
}
