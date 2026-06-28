import { getDb } from '../store/db.js'
import type {
  RemoteImMessage,
  RemoteImMessageDirection,
  RemoteImMessageRole,
  RemoteImMessageStatus,
  RemoteImProvider
} from './types.js'

interface PreparedStatement {
  run: (...args: unknown[]) => { lastInsertRowid?: number | bigint }
  get: (...args: unknown[]) => unknown
  all: (...args: unknown[]) => unknown[]
}

export interface RemoteImDatabase {
  prepare(sql: string): PreparedStatement
}

interface RemoteImMessageRow {
  id: number
  project_id: string | null
  session_id: string | null
  provider: RemoteImProvider
  remote_message_id: string | null
  from_user_id: string | null
  to_user_id: string | null
  role: RemoteImMessageRole
  direction: RemoteImMessageDirection
  content: string
  status: RemoteImMessageStatus
  error: string | null
  created_at: number
  sent_to_aicli_at: number | null
  sent_to_im_at: number | null
}

export interface CreateRemoteImMessageInput {
  projectId: string | null
  sessionId?: string | null
  provider: RemoteImProvider
  remoteMessageId?: string | null
  fromUserId?: string | null
  toUserId?: string | null
  role: RemoteImMessageRole
  direction: RemoteImMessageDirection
  content: string
  status: RemoteImMessageStatus
  error?: string | null
  createdAt?: number
  sentToAicliAt?: number | null
  sentToImAt?: number | null
}

export interface UpdateRemoteImMessageStatusInput {
  status: RemoteImMessageStatus
  sessionId?: string | null
  error?: string | null
  sentToAicliAt?: number | null
  sentToImAt?: number | null
}

function mapRow(row: RemoteImMessageRow): RemoteImMessage {
  return {
    id: row.id,
    projectId: row.project_id,
    sessionId: row.session_id,
    provider: row.provider,
    remoteMessageId: row.remote_message_id,
    fromUserId: row.from_user_id,
    toUserId: row.to_user_id,
    role: row.role,
    direction: row.direction,
    content: row.content,
    status: row.status,
    error: row.error,
    createdAt: row.created_at,
    sentToAicliAt: row.sent_to_aicli_at,
    sentToImAt: row.sent_to_im_at
  }
}

function hasInputKey<T extends object, K extends keyof T>(input: T, key: K): boolean {
  return Object.prototype.hasOwnProperty.call(input, key)
}

export function createRemoteImMessageStore(database: RemoteImDatabase) {
  function listById(id: number): RemoteImMessage | null {
    const row = database
      .prepare('SELECT * FROM remote_im_messages WHERE id = ?')
      .get(id) as RemoteImMessageRow | undefined
    return row ? mapRow(row) : null
  }

  function create(input: CreateRemoteImMessageInput): RemoteImMessage {
    const createdAt = input.createdAt ?? Date.now()
    const result = database
      .prepare(
        `
        INSERT INTO remote_im_messages (
          project_id,
          session_id,
          provider,
          remote_message_id,
          from_user_id,
          to_user_id,
          role,
          direction,
          content,
          status,
          error,
          created_at,
          sent_to_aicli_at,
          sent_to_im_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        `
      )
      .run(
        input.projectId,
        input.sessionId ?? null,
        input.provider,
        input.remoteMessageId ?? null,
        input.fromUserId ?? null,
        input.toUserId ?? null,
        input.role,
        input.direction,
        input.content,
        input.status,
        input.error ?? null,
        createdAt,
        input.sentToAicliAt ?? null,
        input.sentToImAt ?? null
      )
    return listById(Number(result.lastInsertRowid))!
  }

  function list(projectId: string, limit = 100): RemoteImMessage[] {
    const rows = database
      .prepare(
        `
        SELECT *
        FROM (
          SELECT *
          FROM remote_im_messages
          WHERE project_id = ?
          ORDER BY created_at DESC, id DESC
          LIMIT ?
        )
        ORDER BY created_at ASC, id ASC
        `
      )
      .all(projectId, Math.max(1, Math.min(500, Math.round(limit)))) as RemoteImMessageRow[]
    return rows.map(mapRow)
  }

  function updateStatus(
    id: number,
    input: UpdateRemoteImMessageStatusInput
  ): RemoteImMessage | null {
    const current = listById(id)
    if (!current) return null
    database
      .prepare(
        `
        UPDATE remote_im_messages
        SET session_id = ?,
            status = ?,
            error = ?,
            sent_to_aicli_at = ?,
            sent_to_im_at = ?
        WHERE id = ?
        `
      )
      .run(
        hasInputKey(input, 'sessionId') ? input.sessionId ?? null : current.sessionId,
        input.status,
        hasInputKey(input, 'error') ? input.error ?? null : current.error,
        hasInputKey(input, 'sentToAicliAt')
          ? input.sentToAicliAt ?? null
          : current.sentToAicliAt,
        hasInputKey(input, 'sentToImAt') ? input.sentToImAt ?? null : current.sentToImAt,
        id
      )
    return listById(id)
  }

  function failIfStreaming(id: number, error: string): RemoteImMessage | null {
    const current = listById(id)
    if (!current || current.status !== 'streaming') return current
    return updateStatus(id, {
      status: 'failed',
      error: error || 'Remote IM message delivery was not confirmed'
    })
  }

  function clear(projectId: string): void {
    database.prepare('DELETE FROM remote_im_messages WHERE project_id = ?').run(projectId)
  }

  return {
    create,
    listById,
    list,
    updateStatus,
    failIfStreaming,
    clear
  }
}

function defaultStore() {
  return createRemoteImMessageStore(getDb() as unknown as RemoteImDatabase)
}

export function createRemoteImMessage(input: CreateRemoteImMessageInput): RemoteImMessage {
  return defaultStore().create(input)
}

export function listRemoteImMessageById(id: number): RemoteImMessage | null {
  return defaultStore().listById(id)
}

export function listRemoteImMessages(projectId: string, limit = 100): RemoteImMessage[] {
  return defaultStore().list(projectId, limit)
}

export function updateRemoteImMessageStatus(
  id: number,
  input: UpdateRemoteImMessageStatusInput
): RemoteImMessage | null {
  return defaultStore().updateStatus(id, input)
}

export function failRemoteImMessageIfStreaming(id: number, error: string): RemoteImMessage | null {
  return defaultStore().failIfStreaming(id, error)
}

export function clearRemoteImMessages(projectId: string): void {
  defaultStore().clear(projectId)
}
