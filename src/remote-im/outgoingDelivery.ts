import type { TencentImRuntime } from './tencentImClient.js'

export interface RemoteImOutgoingTextEvent {
  projectId: string
  toUserId: string
  text: string
  messageId?: number | null
}

export interface RemoteImOutgoingImageEvent {
  projectId: string
  toUserId: string
  fileToken?: string | null
  fileName?: string | null
  mimeType?: string | null
  fileBytes?: Uint8Array | ArrayBuffer | number[] | null
  messageId?: number | null
}

export interface RemoteImOutgoingFileEvent {
  projectId: string
  toUserId: string
  fileName?: string | null
  mimeType?: string | null
  fileBytes?: Uint8Array | ArrayBuffer | number[] | null
  messageId?: number | null
}

export interface DeliverRemoteImOutgoingTextInput {
  runtime: TencentImRuntime | null
  event: RemoteImOutgoingTextEvent
  markSent(messageId: number, remoteMessageId?: string | null): Promise<unknown> | unknown
  markFailed(messageId: number, error: string): Promise<unknown> | unknown
  sendTimeoutMs?: number
}

export interface DeliverRemoteImOutgoingImageInput {
  runtime: TencentImRuntime | null
  event: RemoteImOutgoingImageEvent
  resolveFile(event: RemoteImOutgoingImageEvent): File | null
  markSent(messageId: number, remoteMessageId?: string | null): Promise<unknown> | unknown
  markFailed(messageId: number, error: string): Promise<unknown> | unknown
  sendTimeoutMs?: number
}

export interface DeliverRemoteImOutgoingFileInput {
  runtime: TencentImRuntime | null
  event: RemoteImOutgoingFileEvent
  markSent(messageId: number, remoteMessageId?: string | null): Promise<unknown> | unknown
  markFailed(messageId: number, error: string): Promise<unknown> | unknown
  sendTimeoutMs?: number
}

const DEFAULT_SEND_TIMEOUT_MS = 15_000

function withTimeout<T>(promise: Promise<T>, timeoutMs: number): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => {
      reject(new Error('IM 发送超时'))
    }, timeoutMs)
    promise.then(
      (value) => {
        clearTimeout(timer)
        resolve(value)
      },
      (error) => {
        clearTimeout(timer)
        reject(error)
      }
    )
  })
}

export async function deliverRemoteImOutgoingText(
  input: DeliverRemoteImOutgoingTextInput
): Promise<void> {
  if (!input.event.messageId) {
    await input.runtime?.sendText(input.event.toUserId, input.event.text, {
      messageId: input.event.messageId
    })
    return
  }

  if (!input.runtime) {
    await input.markFailed(input.event.messageId, 'IM 运行时未连接')
    return
  }

  try {
    const sendResult = await withTimeout(
      input.runtime.sendText(input.event.toUserId, input.event.text, {
        messageId: input.event.messageId
      }),
      input.sendTimeoutMs ?? DEFAULT_SEND_TIMEOUT_MS
    )
    // SDK 确认的消息 ID 回填出站记录：漫游重投同一条消息时按 remote_message_id 去重。
    await input.markSent(input.event.messageId, sendResult?.remoteMessageId ?? null)
  } catch (err) {
    await input.markFailed(
      input.event.messageId,
      err instanceof Error ? err.message : String(err)
    )
  }
}

export async function deliverRemoteImOutgoingImage(
  input: DeliverRemoteImOutgoingImageInput
): Promise<void> {
  if (!input.event.messageId) {
    const file = resolveOutgoingImageFile(input)
    if (file && input.runtime?.sendImage) {
      await input.runtime.sendImage(input.event.toUserId, file, {
        messageId: input.event.messageId
      })
    }
    return
  }

  if (!input.runtime?.sendImage) {
    await input.markFailed(input.event.messageId, 'IM 运行时未连接')
    return
  }

  const file = resolveOutgoingImageFile(input)
  if (!file) {
    await input.markFailed(input.event.messageId, '图片文件已失效，请重新选择')
    return
  }

  try {
    const sendResult = await withTimeout(
      input.runtime.sendImage(input.event.toUserId, file, {
        messageId: input.event.messageId
      }),
      input.sendTimeoutMs ?? DEFAULT_SEND_TIMEOUT_MS
    )
    // SDK 确认的消息 ID 回填出站记录：漫游重投同一条消息时按 remote_message_id 去重。
    await input.markSent(input.event.messageId, sendResult?.remoteMessageId ?? null)
  } catch (err) {
    await input.markFailed(
      input.event.messageId,
      err instanceof Error ? err.message : String(err)
    )
  }
}

export async function deliverRemoteImOutgoingFile(
  input: DeliverRemoteImOutgoingFileInput
): Promise<void> {
  if (!input.event.messageId) {
    const file = resolveOutgoingFile(input.event)
    if (file && input.runtime?.sendFile) {
      await input.runtime.sendFile(input.event.toUserId, file, {
        messageId: input.event.messageId
      })
    }
    return
  }

  if (!input.runtime?.sendFile) {
    await input.markFailed(input.event.messageId, 'IM 运行时未连接')
    return
  }

  const file = resolveOutgoingFile(input.event)
  if (!file) {
    await input.markFailed(input.event.messageId, '文件已失效，请重新选择')
    return
  }

  try {
    const sendResult = await withTimeout(
      input.runtime.sendFile(input.event.toUserId, file, {
        messageId: input.event.messageId
      }),
      input.sendTimeoutMs ?? DEFAULT_SEND_TIMEOUT_MS
    )
    // SDK 确认的消息 ID 回填出站记录：漫游重投同一条消息时按 remote_message_id 去重。
    await input.markSent(input.event.messageId, sendResult?.remoteMessageId ?? null)
  } catch (err) {
    await input.markFailed(
      input.event.messageId,
      err instanceof Error ? err.message : String(err)
    )
  }
}

function resolveOutgoingImageFile(input: DeliverRemoteImOutgoingImageInput): File | null {
  const file = input.resolveFile(input.event)
  if (file) return file
  const bytes = input.event.fileBytes
  if (!bytes) return null
  const fileName = input.event.fileName?.trim() || 'remote-im-image.png'
  const mimeType = input.event.mimeType?.trim() || 'application/octet-stream'
  return new File([toFileArrayBuffer(bytes)], fileName, { type: mimeType })
}

function toFileArrayBuffer(bytes: Uint8Array | ArrayBuffer | number[]): ArrayBuffer {
  if (bytes instanceof ArrayBuffer) return bytes.slice(0)
  const view = Array.isArray(bytes) ? new Uint8Array(bytes) : bytes
  const copy = new ArrayBuffer(view.byteLength)
  new Uint8Array(copy).set(view)
  return copy
}

function resolveOutgoingFile(event: RemoteImOutgoingFileEvent): File | null {
  const bytes = event.fileBytes
  if (!bytes) return null
  const fileName = event.fileName?.trim() || 'remote-im-file.md'
  const mimeType = event.mimeType?.trim() || 'application/octet-stream'
  return new File([toFileArrayBuffer(bytes)], fileName, { type: mimeType })
}
