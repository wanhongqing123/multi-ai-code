import { randomUUID } from 'node:crypto'
import net from 'node:net'

export type AicliStructuredOutputProvider = 'codex' | 'opencode'
export type AicliControlMode = 'plan' | 'build'

export interface AicliStructuredOutputEvent {
  sessionId: string
  provider: AicliStructuredOutputProvider
  kind?: string
  text: string
  messageId?: string
  partId?: string
}

export interface AicliStructuredOutputBridge {
  endpoint: string
  args: string[]
  requestControlCommand(
    input: AicliRequestControlCommand,
    timeoutMs?: number
  ): Promise<AicliControlCommandResult>
  close(): Promise<void>
}

type AicliStructuredOutputListener = (event: AicliStructuredOutputEvent) => void

interface WireEvent {
  token?: unknown
  kind?: unknown
  text?: unknown
  messageId?: unknown
  partId?: unknown
  command?: unknown
  mode?: unknown
  requestId?: unknown
  ok?: unknown
  error?: unknown
  model?: unknown
  reasoning?: unknown
  goal?: unknown
  task?: unknown
  replyId?: unknown
}

// 所有控制命令统一走 requestId RPC：switch_mode 也等待 AICLI 回 control_result，
// 避免“字节写进 socket 就报成功”而 TUI 侧实际拒绝（协作模式未启用等）的假成功。
export type AicliRequestControlCommand =
  | { command: 'status' }
  | { command: 'model'; model?: string; reasoning?: string }
  | { command: 'goal'; goal?: string }
  | { command: 'btw'; task: string; replyId?: string }
  | { command: 'interrupt' }
  | { command: 'compact' }
  | { command: 'clear' }
  | { command: 'switch_mode'; mode: AicliControlMode }

export type AicliControlCommandResult =
  | { ok: true; text: string }
  | { ok: false; error: string; text?: string }

const listeners = new Set<AicliStructuredOutputListener>()

export function addAicliStructuredOutputListener(
  listener: AicliStructuredOutputListener
): () => void {
  listeners.add(listener)
  return () => listeners.delete(listener)
}

function emitStructuredOutput(event: AicliStructuredOutputEvent): void {
  for (const listener of listeners) {
    try {
      listener(event)
    } catch {
      /* keep AICLI output observers isolated */
    }
  }
}

function asOptionalString(value: unknown): string | undefined {
  return typeof value === 'string' && value.trim() ? value : undefined
}

export async function createAicliStructuredOutputBridge(
  sessionId: string,
  provider: AicliStructuredOutputProvider
): Promise<AicliStructuredOutputBridge> {
  const token = randomUUID()
  const controlSockets = new Set<net.Socket>()
  const pendingControlRequests = new Map<
    string,
    {
      resolve: (result: AicliControlCommandResult) => void
      timeout: NodeJS.Timeout
    }
  >()
  const server = net.createServer((socket) => {
    socket.setEncoding('utf8')
    let buffer = ''
    socket.on('close', () => {
      controlSockets.delete(socket)
    })
    socket.on('error', () => {
      controlSockets.delete(socket)
    })

    socket.on('data', (chunk) => {
      buffer += chunk
      for (;;) {
        const lineEnd = buffer.indexOf('\n')
        if (lineEnd < 0) break
        const rawLine = buffer.slice(0, lineEnd).trim()
        buffer = buffer.slice(lineEnd + 1)
        if (!rawLine) continue

        let parsed: WireEvent
        try {
          parsed = JSON.parse(rawLine) as WireEvent
        } catch {
          continue
        }
        if (parsed.token !== token) continue
        if (parsed.kind === 'control_ready') {
          controlSockets.add(socket)
          continue
        }
        if (parsed.kind === 'control_result') {
          const requestId = asOptionalString(parsed.requestId)
          const pending = requestId ? pendingControlRequests.get(requestId) : undefined
          if (!requestId || !pending) continue
          pendingControlRequests.delete(requestId)
          clearTimeout(pending.timeout)
          const text = typeof parsed.text === 'string' ? parsed.text : ''
          if (parsed.ok === true) {
            pending.resolve({ ok: true, text })
          } else {
            pending.resolve({
              ok: false,
              error:
                typeof parsed.error === 'string' && parsed.error.trim()
                  ? parsed.error
                  : 'AICLI control command failed',
              ...(text ? { text } : {})
            })
          }
          continue
        }
        if (typeof parsed.text !== 'string') continue
        if (!parsed.text) continue

        const messageId = asOptionalString(parsed.messageId)
        emitStructuredOutput({
          sessionId,
          provider,
          text: parsed.text,
          kind: asOptionalString(parsed.kind),
          messageId,
          partId: asOptionalString(parsed.partId)
        })

        // 回执：AICLI 侧靠这条 ack 判定数据连接“还活着”。收不到 ack（半死 socket、
        // write 写进黑洞）时 AICLI 会重连并补发，从根上消除“回传一旦丢就一直丢、
        // 必须重启 AICLI”的粘滞故障。只有带 messageId 的 assistant_text 需要回执。
        if (messageId && !socket.destroyed) {
          socket.write(`${JSON.stringify({ token, kind: 'ack', messageId })}\n`)
        }
      }
    })
  })

  await new Promise<void>((resolve, reject) => {
    const onError = (err: Error) => {
      server.off('listening', onListening)
      reject(err)
    }
    const onListening = () => {
      server.off('error', onError)
      resolve()
    }
    server.once('error', onError)
    server.once('listening', onListening)
    server.listen(0, '127.0.0.1')
  })

  const address = server.address()
  if (!address || typeof address === 'string') {
    server.close()
    throw new Error('failed to bind AICLI structured output bridge')
  }

  const endpoint = `tcp://127.0.0.1:${address.port}?token=${encodeURIComponent(token)}`

  function writeControlPayload(payload: object): number {
    const line = `${JSON.stringify({ token, kind: 'control', ...payload })}\n`
    let sent = 0
    for (const socket of Array.from(controlSockets)) {
      if (socket.destroyed) {
        controlSockets.delete(socket)
        continue
      }
      socket.write(line)
      sent += 1
    }
    return sent
  }

  return {
    endpoint,
    args: ['--multi-ai-code-im-ipc', endpoint],
    requestControlCommand: (input, timeoutMs = 5000) => {
      const requestId = randomUUID()
      return new Promise<AicliControlCommandResult>((resolve) => {
        const timeout = setTimeout(() => {
          pendingControlRequests.delete(requestId)
          resolve({ ok: false, error: 'AICLI control command timed out' })
        }, timeoutMs)
        pendingControlRequests.set(requestId, { resolve, timeout })
        const sent = writeControlPayload({
          requestId,
          command: input.command,
          ...(input.command === 'switch_mode' ? { mode: input.mode } : {}),
          ...(input.command === 'model' && input.model ? { model: input.model } : {}),
          ...(input.command === 'model' && input.reasoning ? { reasoning: input.reasoning } : {}),
          ...(input.command === 'btw'
            ? { task: input.task, ...(input.replyId ? { replyId: input.replyId } : {}) }
            : {}),
          ...(input.command === 'goal' && input.goal ? { goal: input.goal } : {})
        })
        if (sent > 0) return
        clearTimeout(timeout)
        pendingControlRequests.delete(requestId)
        resolve({ ok: false, error: 'AICLI control bridge is not connected' })
      })
    },
    close: () =>
      new Promise<void>((resolve) => {
        for (const pending of pendingControlRequests.values()) {
          clearTimeout(pending.timeout)
          pending.resolve({ ok: false, error: 'AICLI control bridge closed' })
        }
        pendingControlRequests.clear()
        for (const socket of controlSockets) socket.destroy()
        controlSockets.clear()
        server.close(() => resolve())
      })
  }
}
