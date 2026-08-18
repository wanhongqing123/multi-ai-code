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

// 扩展名以发送方给的文件名为准。以前只认能映射回已知 MIME 的扩展名，
// 于是 .xlsx / .dwg / .parquet 这类会被丢掉，最后统一落成 .md 或 .bin——
// 用户拿到手的文件双击打不开。任何形似扩展名的后缀都直接沿用。
const EXTENSION_PATTERN = /^\.[A-Za-z0-9]{1,16}$/

function normalizeDocumentExtension(input: {
  fileName?: string | null
  remoteUrl: string
  mimeType?: string | null
}): string {
  const fileExtension = input.fileName ? extname(input.fileName).toLowerCase() : ''
  if (EXTENSION_PATTERN.test(fileExtension)) return fileExtension
  const urlExtension = extensionFromUrl(input.remoteUrl)
  if (urlExtension && EXTENSION_PATTERN.test(urlExtension)) return urlExtension
  const mimeType = input.mimeType?.toLowerCase()
  if (mimeType === 'text/html') return '.html'
  if (mimeType === 'text/markdown') return '.md'
  // 既没有扩展名也认不出 MIME：落 .bin，别猜成 .md 把二进制标成文本。
  return '.bin'
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

  // 不再按 MIME 拒收。接收任意普通文件与发送侧（send-file）对齐；
  // 真正的护栏是体积上限，而不是类型白名单。
  const declaredMimeType = input.mimeType?.split(';')[0]?.trim() || null

  const response = await fetchImpl(input.remoteUrl)
  if (!response.ok) {
    throw new Error(`文件下载失败：HTTP ${response.status}`)
  }

  const bytes = Buffer.from(await response.arrayBuffer())
  const maxBytes = input.maxBytes ?? MAX_REMOTE_IM_FILE_BYTES
  if (bytes.byteLength > maxBytes) throw new Error('file is too large')

  const responseMimeType = response.headers?.get('content-type')?.split(';')[0]?.trim() || null
  // 声明的 MIME 优先于响应头：对象存储对未知类型常年回 application/octet-stream，
  // 用它覆盖发送方声明的类型只会把信息变少。两个都没有时按文件名推。
  const mimeType =
    declaredMimeType ||
    responseMimeType ||
    (input.fileName ? mimeTypeFromRemoteImFilePath(input.fileName) : null)

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
