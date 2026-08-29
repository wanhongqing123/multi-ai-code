import { getDb } from '../store/db.js'
import type {
  RemoteImImageAttachment,
  RemoteImFileAttachment,
  RemoteImMessage,
  RemoteImMessageAttachment,
  RemoteImMessageDirection,
  RemoteImMessageKind,
  RemoteImMessageRole,
  RemoteImMessageStatus,
  RemoteImProvider,
  RemoteImVideoAttachment
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
  kind?: RemoteImMessageKind | string | null
  attachment_json?: string | null
  caption?: string | null
  caption_above?: number | null
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
  kind?: RemoteImMessageKind
  attachment?: RemoteImMessageAttachment | null
  caption?: string | null
  captionAbove?: boolean
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
  // 出站消息发送成功后回填 SDK 确认的消息 id：漫游重投同一条消息时才能按
  // (provider, remote_message_id) 去重。不传则保持原值。
  remoteMessageId?: string | null
}

function normalizeMessageKind(value: unknown): RemoteImMessageKind {
  return value === 'image' || value === 'file' || value === 'video' ? value : 'text'
}

function nullableString(value: unknown): string | null {
  return typeof value === 'string' && value.trim() ? value.trim() : null
}

function nullableNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

function parseImageAttachment(value: Record<string, unknown>): RemoteImImageAttachment {
  return {
    type: 'image',
    localPath: nullableString(value.localPath),
    remoteUrl: nullableString(value.remoteUrl),
    thumbnailUrl: nullableString(value.thumbnailUrl),
    width: nullableNumber(value.width),
    height: nullableNumber(value.height),
    sizeBytes: nullableNumber(value.sizeBytes),
    fileName: nullableString(value.fileName),
    mimeType: nullableString(value.mimeType),
    sdkImageId: nullableString(value.sdkImageId)
  }
}

function parseFileAttachment(value: Record<string, unknown>): RemoteImFileAttachment {
  return {
    type: 'file',
    localPath: nullableString(value.localPath),
    remoteUrl: nullableString(value.remoteUrl),
    sizeBytes: nullableNumber(value.sizeBytes),
    fileName: nullableString(value.fileName),
    mimeType: nullableString(value.mimeType),
    sdkFileId: nullableString(value.sdkFileId)
  }
}

function parseVideoAttachment(value: Record<string, unknown>): RemoteImVideoAttachment {
  return {
    type: 'video',
    localPath: nullableString(value.localPath),
    remoteUrl: nullableString(value.remoteUrl),
    thumbnailUrl: nullableString(value.thumbnailUrl),
    width: nullableNumber(value.width),
    height: nullableNumber(value.height),
    durationSeconds: nullableNumber(value.durationSeconds),
    sizeBytes: nullableNumber(value.sizeBytes),
    fileName: nullableString(value.fileName),
    mimeType: nullableString(value.mimeType),
    sdkVideoId: nullableString(value.sdkVideoId)
  }
}

function parseAttachmentJson(
  kind: RemoteImMessageKind,
  value: string | null | undefined
): RemoteImMessageAttachment | null {
  if (!value) return null
  try {
    const parsed = JSON.parse(value) as unknown
    if (!parsed || typeof parsed !== 'object') return null
    if (kind === 'image') return parseImageAttachment(parsed as Record<string, unknown>)
    if (kind === 'file') return parseFileAttachment(parsed as Record<string, unknown>)
    if (kind === 'video') return parseVideoAttachment(parsed as Record<string, unknown>)
    return null
  } catch {
    return null
  }
}

function serializeAttachmentJson(attachment: RemoteImMessageAttachment | null | undefined): string | null {
  return attachment ? JSON.stringify(attachment) : null
}

function mapRow(row: RemoteImMessageRow): RemoteImMessage {
  const kind = normalizeMessageKind(row.kind)
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
    kind,
    attachment: parseAttachmentJson(kind, row.attachment_json),
    caption: nullableString(row.caption),
    captionAbove: row.caption_above === 1,
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
    // 去重：入站消息带 provider 级的 remoteMessageId，SDK 断线重连会下发漫游历史、
    // 对同一条消息再次触发 onMessageReceived；裸 INSERT 会重复入库 → 重复路由执行。
    // remoteMessageId 为 NULL（出站/系统消息）不去重，照常插入。
    const remoteMessageId = input.remoteMessageId ?? null
    if (remoteMessageId !== null) {
      const existed = database
        .prepare('SELECT * FROM remote_im_messages WHERE provider = ? AND remote_message_id = ?')
        .get(input.provider, remoteMessageId) as RemoteImMessageRow | undefined
      if (existed) return mapRow(existed)
    }
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
          kind,
          attachment_json,
          caption,
          caption_above,
          status,
          error,
          created_at,
          sent_to_aicli_at,
          sent_to_im_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
        input.kind ?? 'text',
        serializeAttachmentJson(input.attachment),
        nullableString(input.caption),
        input.captionAbove === true ? 1 : 0,
        input.status,
        input.error ?? null,
        createdAt,
        input.sentToAicliAt ?? null,
        input.sentToImAt ?? null
      )
    return listById(Number(result.lastInsertRowid))!
  }

  // 会话视图按**账号**取，不按项目过滤。
  //
  // 数据库本身就是每个账号一个（accounts/<userId>/multi-ai-code.db），而 project_id
  // 只是记录「这条消息当时被哪个项目的会话处理了」——它来自消息到达那一刻窗口里恰好
  // 打开的项目，不是消息自身的属性。按它过滤，会把同一段 IM 对话按「当时开着哪个仓库」
  // 任意切碎：用户换个仓库就看不到自己刚发过的消息。
  //
  // projectId 参数保留：调用方仍然按项目组织，且它是发送路径的必需信息，
  // 这里只是不再拿它做过滤条件。
  function list(_projectId: string, limit = 100): RemoteImMessage[] {
    const rows = database
      .prepare(
        `
        SELECT *
        FROM (
          SELECT *
          FROM remote_im_messages
          ORDER BY created_at DESC, id DESC
          LIMIT ?
        )
        ORDER BY created_at ASC, id ASC
        `
      )
      .all(Math.max(1, Math.min(500, Math.round(limit)))) as RemoteImMessageRow[]
    return rows.map(mapRow)
  }

  // 汇总视图用：一次取回项目最近的消息全集（升序）。上限独立于 list() 的 500，
  // 但仍设 5000 硬顶避免超大历史一次性拖爆 IPC。
  function listRecent(_projectId: string, limit = 3000): RemoteImMessage[] {
    const rows = database
      .prepare(
        `
        SELECT *
        FROM (
          SELECT *
          FROM remote_im_messages
          ORDER BY created_at DESC, id DESC
          LIMIT ?
        )
        ORDER BY created_at ASC, id ASC
        `
      )
      .all(Math.max(1, Math.min(5000, Math.round(limit)))) as RemoteImMessageRow[]
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
            sent_to_im_at = ?,
            remote_message_id = ?
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
        // remoteMessageId 只增不清：传 null/未传都保持原值，避免误抹掉已有 id。
        input.remoteMessageId ?? current.remoteMessageId ?? null,
        id
      )
    return listById(id)
  }

  // 键集分页取某会话更早的消息：严格早于 (beforeCreatedAt, beforeId)，
  // 升序返回，最多 limit 条。大历史下配合前端「加载更早」按需翻页。
  function listPeerBefore(
    _projectId: string,
    rawPeerUserId: string,
    beforeCreatedAt: number,
    beforeId: number,
    limit: number
  ): RemoteImMessage[] {
    const peerUserId = rawPeerUserId.trim()
    if (!peerUserId || limit <= 0) return []
    const rows = database
      .prepare(
        `
        SELECT *
        FROM remote_im_messages
        WHERE (from_user_id = ? OR to_user_id = ?)
          AND (created_at < ? OR (created_at = ? AND id < ?))
        ORDER BY created_at DESC, id DESC
        LIMIT ?
        `
      )
      .all(
        peerUserId,
        peerUserId,
        beforeCreatedAt,
        beforeCreatedAt,
        beforeId,
        Math.max(1, Math.min(500, Math.round(limit)))
      ) as RemoteImMessageRow[]
    return rows.map(mapRow).reverse()
  }

  function findByRemoteMessageId(
    provider: RemoteImProvider,
    remoteMessageId: string
  ): RemoteImMessage | null {
    const row = database
      .prepare('SELECT * FROM remote_im_messages WHERE provider = ? AND remote_message_id = ?')
      .get(provider, remoteMessageId) as RemoteImMessageRow | undefined
    return row ? mapRow(row) : null
  }

  function failIfStreaming(id: number, error: string): RemoteImMessage | null {
    const current = listById(id)
    if (!current || current.status !== 'streaming') return current
    return updateStatus(id, {
      status: 'failed',
      error: error || 'Remote IM message delivery was not confirmed'
    })
  }

  // 清除范围必须与查看范围一致：会话视图已经是账号级，如果这里还按项目删，
  // 用户点完「清除」仍会看到别的项目留下的同一段对话——界面在骗人。
  function clearPeer(_projectId: string, rawPeerUserId: string): void {
    const peerUserId = rawPeerUserId.trim()
    if (!peerUserId) return
    database
      .prepare(
        `
        DELETE FROM remote_im_messages
        WHERE (from_user_id = ? OR to_user_id = ?)
        `
      )
      .run(peerUserId, peerUserId)
  }

  return {
    create,
    listById,
    list,
    listRecent,
    updateStatus,
    failIfStreaming,
    clearPeer,
    listPeerBefore,
    findByRemoteMessageId
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

export function listRemoteImMessagesForSummary(projectId: string, limit = 3000): RemoteImMessage[] {
  return defaultStore().listRecent(projectId, limit)
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

export function clearRemoteImPeerMessages(projectId: string, peerUserId: string): void {
  defaultStore().clearPeer(projectId, peerUserId)
}

export function listRemoteImPeerMessagesBefore(
  projectId: string,
  peerUserId: string,
  beforeCreatedAt: number,
  beforeId: number,
  limit = 200
): RemoteImMessage[] {
  return defaultStore().listPeerBefore(projectId, peerUserId, beforeCreatedAt, beforeId, limit)
}

export function findRemoteImMessageByRemoteId(
  provider: RemoteImProvider,
  remoteMessageId: string
): RemoteImMessage | null {
  return defaultStore().findByRemoteMessageId(provider, remoteMessageId)
}
