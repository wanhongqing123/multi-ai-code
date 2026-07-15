import { promises as fs } from 'fs'
import { extname, join } from 'path'
import type { RemoteImFileAttachment, RemoteImIncomingFileMessage } from './types.js'
import { mimeTypeFromRemoteImFilePath } from './localFile.js'

export interface RemoteImFileFetchResponse {
  ok: boolean
  status: number
  headers?: { get(name: string): string | null }
  arrayBuffer(): Promise<ArrayBuffer>
}

export type RemoteImFileFetch = (url: string) => Promise<RemoteImFileFetchResponse>

export interface CacheRemoteImFileInput {
  rootDir: string
  projectId: string
  remoteUrl: string
  remoteMessageId?: string | null
  fileName?: string | null
  mimeType?: string | null
  fetchImpl?: RemoteImFileFetch
  maxBytes?: number
}

export interface CachedRemoteImFile {
  localPath: string
  fileName: string
  mimeType: string | null
  sizeBytes: number
}

const MAX_REMOTE_IM_FILE_BYTES = 5 * 1024 * 1024

function sanitizePathPart(value: string, fallback: string): string {
  const sanitized = value
    .trim()
    .replace(/[/\\:]/g, '_')
    .replace(/[^a-zA-Z0-9._-]+/g, '_')
    .replace(/_+/g, '_')
    .replace(/^_+|_+$/g, '')
  return sanitized || fallback
}

function extensionFromUrl(remoteUrl: string): string | null {
  try {
    const extension = extname(new URL(remoteUrl).pathname).toLowerCase()
    return extension || null
  } catch {
    return null
  }
}

function normalizeDocumentExtension(input: {
  fileName?: string | null
  remoteUrl: string
  mimeType?: string | null
}): string {
  const fileExtension = input.fileName ? extname(input.fileName).toLowerCase() : ''
  if (fileExtension && mimeTypeFromRemoteImFilePath(`x${fileExtension}`)) return fileExtension
  const urlExtension = extensionFromUrl(input.remoteUrl)
  if (urlExtension && mimeTypeFromRemoteImFilePath(`x${urlExtension}`)) return urlExtension
  const mimeType = input.mimeType?.toLowerCase()
  return mimeType === 'text/html' ? '.html' : '.md'
}

export function fileAttachmentFromIncoming(
  message: RemoteImIncomingFileMessage,
  patch: Partial<RemoteImFileAttachment> = {}
): RemoteImFileAttachment {
  return {
    type: 'file',
    localPath: patch.localPath ?? null,
    remoteUrl: patch.remoteUrl ?? message.fileUrl.trim(),
    sizeBytes: patch.sizeBytes ?? (Number.isFinite(message.sizeBytes) ? message.sizeBytes ?? null : null),
    fileName: patch.fileName ?? message.fileName?.trim() ?? null,
    mimeType: patch.mimeType ?? message.mimeType?.trim() ?? null,
    sdkFileId: patch.sdkFileId ?? message.uuid?.trim() ?? null
  }
}

export async function cacheRemoteImFile(input: CacheRemoteImFileInput): Promise<CachedRemoteImFile> {
  const fetchImpl = input.fetchImpl ?? (globalThis.fetch as unknown as RemoteImFileFetch)
  if (!fetchImpl) throw new Error('当前运行环境不支持下载文件')

  const declaredMimeType = input.mimeType?.split(';')[0]?.trim() || null
  if (declaredMimeType && declaredMimeType !== 'text/markdown' && declaredMimeType !== 'text/html') {
    throw new Error('unsupported file type')
  }

  const response = await fetchImpl(input.remoteUrl)
  if (!response.ok) {
    throw new Error(`文件下载失败：HTTP ${response.status}`)
  }

  const bytes = Buffer.from(await response.arrayBuffer())
  const maxBytes = input.maxBytes ?? MAX_REMOTE_IM_FILE_BYTES
  if (bytes.byteLength > maxBytes) throw new Error('file is too large')

  const responseMimeType = response.headers?.get('content-type')?.split(';')[0]?.trim() || null
  const mimeType = responseMimeType || declaredMimeType || null
  if (mimeType && mimeType !== 'text/markdown' && mimeType !== 'text/html') {
    throw new Error('unsupported file type')
  }

  const extension = normalizeDocumentExtension({
    fileName: input.fileName,
    remoteUrl: input.remoteUrl,
    mimeType
  })
  const baseName = sanitizePathPart(input.remoteMessageId ?? input.fileName ?? 'file', 'file')
  const fileName = `${baseName}${extension}`
  const directory = join(
    input.rootDir,
    'remote-im',
    'files',
    sanitizePathPart(input.projectId, 'project')
  )
  const localPath = join(directory, fileName)

  await fs.mkdir(directory, { recursive: true })
  await fs.writeFile(localPath, bytes)
  return {
    localPath,
    fileName,
    mimeType,
    sizeBytes: bytes.byteLength
  }
}
