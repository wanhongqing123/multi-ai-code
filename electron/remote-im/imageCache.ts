import { randomUUID } from 'node:crypto'
import { promises as fs } from 'fs'
import { extname, join } from 'path'

export interface RemoteImImageFetchResponse {
  ok: boolean
  status: number
  headers?: { get(name: string): string | null }
  arrayBuffer(): Promise<ArrayBuffer>
}

export type RemoteImImageFetch = (url: string) => Promise<RemoteImImageFetchResponse>

export interface CacheRemoteImImageInput {
  rootDir: string
  projectId: string
  remoteUrl: string
  remoteMessageId?: string | null
  fileName?: string | null
  mimeType?: string | null
  fetchImpl?: RemoteImImageFetch
}

export interface CachedRemoteImImage {
  localPath: string
  fileName: string
  mimeType: string | null
  sizeBytes: number
}

export interface CacheRemoteImImageBytesInput {
  rootDir: string
  projectId: string
  fileName?: string | null
  mimeType?: string | null
  bytes: ArrayBuffer | Uint8Array
}

const MIME_EXTENSIONS: Record<string, string> = {
  'image/jpeg': '.jpg',
  'image/jpg': '.jpg',
  'image/png': '.png',
  'image/gif': '.gif',
  'image/webp': '.webp'
}

const IMAGE_DOWNLOAD_ATTEMPTS = 3
const IMAGE_DOWNLOAD_RETRY_DELAY_MS = 250
const RETRYABLE_IMAGE_NETWORK_CODES = new Set([
  'ERR_FAILED', 'ERR_NETWORK_CHANGED', 'ERR_CONNECTION_RESET', 'ERR_CONNECTION_CLOSED',
  'ERR_CONNECTION_ABORTED', 'ERR_CONNECTION_TIMED_OUT', 'ERR_TIMED_OUT', 'ERR_NAME_NOT_RESOLVED',
  'ERR_INTERNET_DISCONNECTED', 'ERR_PROXY_CONNECTION_FAILED', 'ERR_TUNNEL_CONNECTION_FAILED',
  'ERR_HTTP2_PROTOCOL_ERROR', 'ECONNRESET', 'ECONNREFUSED', 'ETIMEDOUT', 'EAI_AGAIN', 'ENOTFOUND',
  'UND_ERR_SOCKET', 'UND_ERR_CONNECT_TIMEOUT', 'UND_ERR_HEADERS_TIMEOUT', 'UND_ERR_BODY_TIMEOUT'
])

function isRetryableImageDownloadError(error: unknown): boolean {
  if (!(error instanceof Error)) return false
  // Electron 与 Node 使用不同的网络错误；只重试连接故障，不重试 HTTP 拒绝、
  // 无效地址、主动取消或文件写入失败。响应体中途断开也必须重新请求整张图片。
  const chromiumCode = /net::(ERR_[A-Z0-9_]+)/.exec(error.message)?.[1]
  if (chromiumCode && RETRYABLE_IMAGE_NETWORK_CODES.has(chromiumCode)) return true
  const cause = error.cause as { code?: unknown } | undefined
  const code = (error as Error & { code?: unknown }).code ?? cause?.code
  return typeof code === 'string' && RETRYABLE_IMAGE_NETWORK_CODES.has(code)
}

async function downloadRemoteImImage(
  remoteUrl: string,
  fetchImpl: RemoteImImageFetch
): Promise<{ bytes: Buffer; mimeType: string | null }> {
  for (let attempt = 0; ; attempt += 1) {
    try {
      const response = await fetchImpl(remoteUrl)
      if (!response.ok) throw new Error(`图片下载失败：HTTP ${response.status}`)
      const bytes = Buffer.from(await response.arrayBuffer())
      const mimeType = response.headers?.get('content-type')?.split(';')[0]?.trim() || null
      return { bytes, mimeType }
    } catch (error) {
      if (attempt >= IMAGE_DOWNLOAD_ATTEMPTS - 1 || !isRetryableImageDownloadError(error)) throw error
      // 一次短暂网络故障不应立即把消息永久标成失败；总共最多请求三次，
      // 重试仍在原接收/手动恢复操作内部，不重建消息，也不重新投递旧任务。
      await new Promise<void>((resolve) => setTimeout(resolve, IMAGE_DOWNLOAD_RETRY_DELAY_MS * (attempt + 1)))
    }
  }
}

function sanitizePathPart(value: string, fallback: string): string {
  const sanitized = value
    .trim()
    .replace(/[/\\:]/g, '_')
    .replace(/[^a-zA-Z0-9._-]+/g, '_')
    .replace(/_+/g, '_')
    .replace(/^_+|_+$/g, '')
  return sanitized || fallback
}

export function remoteImImageCacheDirectory(rootDir: string, projectId: string): string {
  return join(rootDir, 'remote-im', 'images', sanitizePathPart(projectId, 'project'))
}

export async function cacheRemoteImImageBytes(
  input: CacheRemoteImImageBytesInput
): Promise<CachedRemoteImImage> {
  const bytes =
    input.bytes instanceof ArrayBuffer
      ? Buffer.from(input.bytes)
      : Buffer.from(input.bytes.buffer, input.bytes.byteOffset, input.bytes.byteLength)
  const extension = normalizeImageExtension({
    fileName: input.fileName,
    remoteUrl: '',
    mimeType: input.mimeType
  })
  const storedFileName = `outgoing-${randomUUID()}${extension}`
  const directory = remoteImImageCacheDirectory(input.rootDir, input.projectId)
  const localPath = join(directory, storedFileName)
  await fs.mkdir(directory, { recursive: true })
  await fs.writeFile(localPath, bytes)
  return {
    localPath,
    fileName: input.fileName?.trim() || storedFileName,
    mimeType: input.mimeType?.trim() || null,
    sizeBytes: bytes.byteLength
  }
}

function extensionFromUrl(remoteUrl: string): string | null {
  try {
    const pathname = new URL(remoteUrl).pathname
    const extension = extname(pathname).toLowerCase()
    return extension || null
  } catch {
    return null
  }
}

function normalizeImageExtension(input: {
  fileName?: string | null
  remoteUrl: string
  mimeType?: string | null
}): string {
  const fileExtension = input.fileName ? extname(input.fileName).toLowerCase() : ''
  if (fileExtension) return fileExtension
  const urlExtension = extensionFromUrl(input.remoteUrl)
  if (urlExtension) return urlExtension
  const mimeExtension = input.mimeType ? MIME_EXTENSIONS[input.mimeType.toLowerCase()] : null
  return mimeExtension ?? '.jpg'
}

export async function cacheRemoteImImage(input: CacheRemoteImImageInput): Promise<CachedRemoteImImage> {
  const fetchImpl = input.fetchImpl ?? (globalThis.fetch as unknown as RemoteImImageFetch)
  if (!fetchImpl) throw new Error('当前运行环境不支持下载图片')

  const { bytes, mimeType: responseMimeType } = await downloadRemoteImImage(input.remoteUrl, fetchImpl)
  const mimeType = responseMimeType || input.mimeType?.trim() || null
  const extension = normalizeImageExtension({
    fileName: input.fileName,
    remoteUrl: input.remoteUrl,
    mimeType
  })
  const baseName = sanitizePathPart(input.remoteMessageId ?? input.fileName ?? 'image', 'image')
  const fileName = `${baseName}${extension}`
  const directory = remoteImImageCacheDirectory(input.rootDir, input.projectId)
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
