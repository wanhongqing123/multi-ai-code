import net from 'node:net'
import { describe, expect, it } from 'vitest'
import {
  addAicliStructuredOutputListener,
  createAicliStructuredOutputBridge,
  type AicliStructuredOutputEvent
} from './structuredOutputBridge.js'

function parseTcpEndpoint(endpoint: string): { port: number; token: string } {
  const url = new URL(endpoint)
  return {
    port: Number(url.port),
    token: url.searchParams.get('token') ?? ''
  }
}

function sendLine(port: number, payload: unknown): Promise<void> {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: '127.0.0.1', port }, () => {
      socket.end(`${JSON.stringify(payload)}\n`)
    })
    socket.once('error', reject)
    socket.once('close', () => resolve())
  })
}

describe('AICLI structured output bridge', () => {
  it('accepts token-matched JSONL output for one session', async () => {
    const bridge = await createAicliStructuredOutputBridge('session-1', 'opencode')
    const { port, token } = parseTcpEndpoint(bridge.endpoint)
    const events: AicliStructuredOutputEvent[] = []
    const removeListener = addAicliStructuredOutputListener((event) => {
      events.push(event)
    })

    await sendLine(port, {
      token,
      kind: 'assistant_text',
      text: '<remote-im-reply id="rim-1">\nhello\n</remote-im-reply id="rim-1">',
      messageId: 'm1',
      partId: 'p1'
    })
    await new Promise((resolve) => setTimeout(resolve, 20))

    removeListener()
    await bridge.close()

    expect(bridge.args).toEqual(['--multi-ai-code-im-ipc', bridge.endpoint])
    expect(events).toEqual([
      {
        sessionId: 'session-1',
        provider: 'opencode',
        kind: 'assistant_text',
        text: '<remote-im-reply id="rim-1">\nhello\n</remote-im-reply id="rim-1">',
        messageId: 'm1',
        partId: 'p1'
      }
    ])
  })

  it('ignores invalid tokens and malformed payloads', async () => {
    const bridge = await createAicliStructuredOutputBridge('session-1', 'codex')
    const { port } = parseTcpEndpoint(bridge.endpoint)
    const events: AicliStructuredOutputEvent[] = []
    const removeListener = addAicliStructuredOutputListener((event) => {
      events.push(event)
    })

    await sendLine(port, { token: 'wrong', text: 'should not pass' })
    await sendLine(port, { text: 'missing token' })
    await sendLine(port, { token: 'wrong', text: 123 })
    await new Promise((resolve) => setTimeout(resolve, 20))

    removeListener()
    await bridge.close()

    expect(events).toEqual([])
  })

  it('switches mode via request/response and reports the AICLI verdict', async () => {
    const bridge = await createAicliStructuredOutputBridge('session-1', 'opencode')
    const { port, token } = parseTcpEndpoint(bridge.endpoint)
    const receivedLines: string[] = []

    const socket = await new Promise<net.Socket>((resolve, reject) => {
      const client = net.createConnection({ host: '127.0.0.1', port }, () => {
        client.write(`${JSON.stringify({ token, kind: 'control_ready' })}\n`)
        resolve(client)
      })
      client.setEncoding('utf8')
      client.on('data', (chunk) => {
        receivedLines.push(String(chunk))
        const requestId = String(chunk).match(/"requestId":"([^"]+)"/)?.[1]
        if (requestId) {
          client.write(
            `${JSON.stringify({
              token,
              kind: 'control_result',
              requestId,
              ok: false,
              error: 'Collaboration modes are disabled.'
            })}\n`
          )
        }
      })
      client.once('error', reject)
    })

    await new Promise((resolve) => setTimeout(resolve, 20))
    const result = await bridge.requestControlCommand(
      { command: 'switch_mode', mode: 'plan' },
      500
    )

    socket.destroy()
    await bridge.close()

    // TUI 拒绝切换时，主仓要拿到真实失败而不是“写入即成功”。
    expect(result).toEqual({ ok: false, error: 'Collaboration modes are disabled.' })
    expect(receivedLines.join('')).toContain('"kind":"control"')
    expect(receivedLines.join('')).toContain('"command":"switch_mode"')
    expect(receivedLines.join('')).toContain('"mode":"plan"')
    expect(receivedLines.join('')).toContain('"requestId"')
  })

  it('resolves request/response control commands from token-verified sockets', async () => {
    const bridge = await createAicliStructuredOutputBridge('session-1', 'codex')
    const { port, token } = parseTcpEndpoint(bridge.endpoint)
    const receivedLines: string[] = []

    const socket = await new Promise<net.Socket>((resolve, reject) => {
      const client = net.createConnection({ host: '127.0.0.1', port }, () => {
        client.write(`${JSON.stringify({ token, kind: 'control_ready' })}\n`)
        resolve(client)
      })
      client.setEncoding('utf8')
      client.on('data', (chunk) => {
        receivedLines.push(String(chunk))
        const requestId = String(chunk).match(/"requestId":"([^"]+)"/)?.[1]
        if (requestId) {
          client.write(
            `${JSON.stringify({
              token,
              kind: 'control_result',
              requestId,
              ok: true,
              text: 'OpenAI Codex\nModel gpt-5.6-sol'
            })}\n`
          )
        }
      })
      client.once('error', reject)
    })

    await new Promise((resolve) => setTimeout(resolve, 20))
    const result = await bridge.requestControlCommand({ command: 'status' }, 500)

    socket.destroy()
    await bridge.close()

    expect(result).toEqual({ ok: true, text: 'OpenAI Codex\nModel gpt-5.6-sol' })
    expect(receivedLines.join('')).toContain('"kind":"control"')
    expect(receivedLines.join('')).toContain('"command":"status"')
    expect(receivedLines.join('')).toContain('"requestId"')
  })
})
