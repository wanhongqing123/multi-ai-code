import type { RemoteImImageAttachment } from './types.js'

const IMAGE_EXTENSIONS = new Set(['png', 'jpg', 'jpeg', 'gif', 'webp'])

const IMAGE_MIME_BY_EXTENSION: Record<string, string> = {
  gif: 'image/gif',
  jpeg: 'image/jpeg',
  jpg: 'image/jpeg',
  png: 'image/png',
  webp: 'image/webp'
}

const MARKDOWN_IMAGE_RE = /!\[[^\]]*]\(([^)\n]+)\)/g
const LOCAL_IMAGE_PATH_RE =
  /(^|[\s"'`(:：])((?:\/[^\s"'`，。；;、）)\]]+|[A-Za-z]:[\\/][^\s"'`，。；;、）)\]]+)\.(?:png|jpe?g|gif|webp))(?=$|[\s"'`，。；;、）)\]])/gi

function normalizeImagePathCandidate(value: string): string {
  let candidate = value.trim()
  if (candidate.startsWith('<') && candidate.endsWith('>')) {
    candidate = candidate.slice(1, -1).trim()
  }
  const titleStart = candidate.search(/\s+["']/)
  if (titleStart > 0) candidate = candidate.slice(0, titleStart).trim()
  if (candidate.startsWith('file://')) candidate = candidate.slice('file://'.length)
  try {
    candidate = decodeURI(candidate)
  } catch {
    // Keep the original candidate if it is not URI-encoded cleanly.
  }
  return candidate.replace(/[),.;，。；、]+$/g, '')
}

function imageExtension(path: string): string | null {
  const match = /\.([A-Za-z0-9]+)$/.exec(path.trim())
  if (!match) return null
  const ext = match[1].toLowerCase()
  return IMAGE_EXTENSIONS.has(ext) ? ext : null
}

function isLocalImagePath(path: string): boolean {
  if (/^https?:\/\//i.test(path)) return false
  if (path.startsWith('//')) return false
  if (!path.startsWith('/') && !/^[A-Za-z]:[\\/]/.test(path)) return false
  return imageExtension(path) !== null
}

function fileNameFromPath(path: string): string | null {
  const fileName = path.split(/[\\/]/).filter(Boolean).pop()?.trim()
  return fileName || null
}

export function mimeTypeFromRemoteImImagePath(path: string): string | null {
  const ext = imageExtension(path)
  return ext ? IMAGE_MIME_BY_EXTENSION[ext] ?? null : null
}

export function createRemoteImImageAttachmentFromLocalPath(
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

export function extractRemoteImAicliImagePaths(text: string): string[] {
  const result: string[] = []
  const seen = new Set<string>()
  const addCandidate = (raw: string): void => {
    const candidate = normalizeImagePathCandidate(raw)
    if (!isLocalImagePath(candidate) || seen.has(candidate)) return
    seen.add(candidate)
    result.push(candidate)
  }

  for (const match of text.matchAll(MARKDOWN_IMAGE_RE)) {
    addCandidate(match[1] ?? '')
  }
  for (const match of text.matchAll(LOCAL_IMAGE_PATH_RE)) {
    addCandidate(match[2] ?? '')
  }
  return result
}
