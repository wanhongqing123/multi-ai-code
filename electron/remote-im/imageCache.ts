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

  const response = await fetchImpl(input.remoteUrl)
  if (!response.ok) {
    throw new Error(`图片下载失败：HTTP ${response.status}`)
  }

  const bytes = Buffer.from(await response.arrayBuffer())
  const responseMimeType = response.headers?.get('content-type')?.split(';')[0]?.trim() || null
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
