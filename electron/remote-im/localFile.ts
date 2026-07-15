import { promises as fs } from 'node:fs'
import type { RemoteImFileAttachment } from './types.js'

const DOCUMENT_EXTENSIONS = new Set(['md', 'markdown', 'html', 'htm'])

const DOCUMENT_MIME_BY_EXTENSION: Record<string, string> = {
  htm: 'text/html',
  html: 'text/html',
  markdown: 'text/markdown',
  md: 'text/markdown'
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

function documentExtension(path: string): string | null {
  const match = /\.([A-Za-z0-9]+)$/.exec(path.trim())
  if (!match) return null
  const ext = match[1].toLowerCase()
  return DOCUMENT_EXTENSIONS.has(ext) ? ext : null
}

export function mimeTypeFromRemoteImFilePath(path: string): string | null {
  const ext = documentExtension(path)
  return ext ? DOCUMENT_MIME_BY_EXTENSION[ext] ?? null : null
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
  if (!mimeType) throw new Error('unsupported file type')

  const stat = await fs.stat(cleanPath)
  if (!stat.isFile()) throw new Error('file path is not a file')
  if (stat.size > options.maxBytes) throw new Error('file is too large')

  const attachment = createRemoteImFileAttachmentFromLocalPath(cleanPath, stat.size)
  const bytes = await fs.readFile(cleanPath)
  return {
    attachment,
    fileName: attachment.fileName ?? 'remote-im-file.md',
    mimeType,
    fileBytes: toTransferableArrayBuffer(bytes)
  }
}
