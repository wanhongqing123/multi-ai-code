import { randomUUID } from 'node:crypto'
import net from 'node:net'

export type AicliStructuredOutputProvider = 'codex' | 'opencode'

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
  close(): Promise<void>
}

type AicliStructuredOutputListener = (event: AicliStructuredOutputEvent) => void

interface WireEvent {
  token?: unknown
  kind?: unknown
  text?: unknown
  messageId?: unknown
  partId?: unknown
}

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
  const server = net.createServer((socket) => {
    socket.setEncoding('utf8')
    let buffer = ''

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
        if (parsed.token !== token || typeof parsed.text !== 'string') continue
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

  return {
    endpoint,
    args: ['--multi-ai-code-im-ipc', endpoint],
    close: () =>
      new Promise<void>((resolve) => {
        server.close(() => resolve())
      })
  }
}
