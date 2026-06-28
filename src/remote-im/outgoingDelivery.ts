import type { TencentImRuntime } from './tencentImClient.js'

export interface RemoteImOutgoingTextEvent {
  projectId: string
  toUserId: string
  text: string
  messageId?: number | null
}

export interface DeliverRemoteImOutgoingTextInput {
  runtime: TencentImRuntime | null
  event: RemoteImOutgoingTextEvent
  markSent(messageId: number): Promise<unknown> | unknown
  markFailed(messageId: number, error: string): Promise<unknown> | unknown
  sendTimeoutMs?: number
}

const DEFAULT_SEND_TIMEOUT_MS = 15_000

function withTimeout<T>(promise: Promise<T>, timeoutMs: number): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => {
      reject(new Error('Tencent IM send timed out'))
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
    await input.markFailed(input.event.messageId, 'Tencent IM runtime is not connected')
    return
  }

  try {
    await withTimeout(
      input.runtime.sendText(input.event.toUserId, input.event.text, {
        messageId: input.event.messageId
      }),
      input.sendTimeoutMs ?? DEFAULT_SEND_TIMEOUT_MS
    )
    await input.markSent(input.event.messageId)
  } catch (err) {
    await input.markFailed(
      input.event.messageId,
      err instanceof Error ? err.message : String(err)
    )
  }
}
