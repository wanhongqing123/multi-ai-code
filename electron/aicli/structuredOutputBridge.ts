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
  sendControlCommand(input: AicliControlCommand): { ok: boolean; error?: string }
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
}

export type AicliControlCommand = {
  command: 'switch_mode'
  mode: AicliControlMode
}

export type AicliRequestControlCommand = {
  command: 'status'
}

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

        emitStructuredOutput({
          sessionId,
          provider,
          text: parsed.text,
          kind: asOptionalString(parsed.kind),
          messageId: asOptionalString(parsed.messageId),
          partId: asOptionalString(parsed.partId)
        })
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
    sendControlCommand: (input) => {
      const sent = writeControlPayload({
        command: input.command,
        mode: input.mode
      })
      return sent > 0
        ? { ok: true }
        : { ok: false, error: 'AICLI control bridge is not connected' }
    },
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
          command: input.command
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
