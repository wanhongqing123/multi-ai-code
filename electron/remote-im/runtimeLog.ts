import { promises as fs } from 'fs'
import { join } from 'path'
import type { RemoteImRuntimeLogEntry, RemoteImRuntimeLogEntryInput } from './types.js'

const RUNTIME_LOG_FILE_NAME = 'remote-im-runtime.log'
const REDACTED_VALUE = '[redacted]'

function isCredentialLikeKey(key: string): boolean {
  const normalized = key.toLowerCase()
  return (
    normalized.includes('usersig') ||
    normalized.includes('secretkey') ||
    normalized === 'secret' ||
    normalized.includes('password') ||
    normalized.includes('token')
  )
}

function sanitizeDetail(value: unknown, depth = 0): unknown {
  if (depth > 6) return '[depth-limit]'
  if (value === null || typeof value !== 'object') return value
  if (Array.isArray(value)) return value.map((item) => sanitizeDetail(item, depth + 1))
  return Object.fromEntries(
    Object.entries(value as Record<string, unknown>).map(([key, item]) => [
      key,
      isCredentialLikeKey(key) ? REDACTED_VALUE : sanitizeDetail(item, depth + 1)
    ])
  )
}

export function normalizeRemoteImRuntimeLogEntry(
  input: RemoteImRuntimeLogEntryInput
): RemoteImRuntimeLogEntry {
  return {
    projectId: input.projectId ?? null,
    sdkAppId:
      typeof input.sdkAppId === 'number' && Number.isInteger(input.sdkAppId)
        ? input.sdkAppId
        : null,
    desktopUserId: input.desktopUserId?.trim() || null,
    peerUserId: input.peerUserId?.trim() || null,
    messageId:
      typeof input.messageId === 'number' && Number.isInteger(input.messageId)
        ? input.messageId
        : null,
    event: input.event.trim() || 'unknown',
    detail: sanitizeDetail(input.detail),
    createdAt:
      typeof input.createdAt === 'number' && Number.isFinite(input.createdAt)
        ? input.createdAt
        : Date.now()
  }
}

export async function appendRemoteImRuntimeLog(
  root: string,
  input: RemoteImRuntimeLogEntryInput
): Promise<RemoteImRuntimeLogEntry> {
  const entry = normalizeRemoteImRuntimeLogEntry(input)
  await fs.mkdir(root, { recursive: true })
  await fs.appendFile(join(root, RUNTIME_LOG_FILE_NAME), `${JSON.stringify(entry)}\n`, 'utf8')
  return entry
}
