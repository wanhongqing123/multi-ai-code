import net from 'node:net'
import { describe, expect, it } from 'vitest'
import {
  addAicliStructuredOutputListener,
  createAicliStructuredOutputBridge,
  type AicliStructuredOutputEvent
} from '../../../electron/aicli/structuredOutputBridge.js'

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
  it('accepts token-matched JSONL output and acks it on the same socket', async () => {
    const bridge = await createAicliStructuredOutputBridge('session-1', 'opencode')
    const { port, token } = parseTcpEndpoint(bridge.endpoint)
    const events: AicliStructuredOutputEvent[] = []
    const removeListener = addAicliStructuredOutputListener((event) => {
      events.push(event)
    })

    // 生产环境的数据 socket 是持久、双向的：AICLI 发 assistant_text，宿主在同一条
    // socket 上回 ack。用持久 socket 测试并断言拿到 ack。
    let acked = ''
    const socket = await new Promise<net.Socket>((resolve, reject) => {
      const client = net.createConnection({ host: '127.0.0.1', port }, () => resolve(client))
      client.setEncoding('utf8')
      client.on('data', (chunk) => {
        acked += String(chunk)
      })
      client.once('error', reject)
    })

    socket.write(
      `${JSON.stringify({
        token,
        kind: 'assistant_text',
        text: '<remote-im-reply id="rim-1">\nhello\n</remote-im-reply id="rim-1">',
        messageId: 'm1',
        partId: 'p1'
      })}\n`
    )
    await new Promise((resolve) => setTimeout(resolve, 30))

    socket.destroy()
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
    expect(acked).toContain('"kind":"ack"')
    expect(acked).toContain('"messageId":"m1"')
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

  it('reports source-level readiness after the AICLI control bridge connects', async () => {
    const bridge = await createAicliStructuredOutputBridge('session-1', 'codex')
    const { port, token } = parseTcpEndpoint(bridge.endpoint)

    expect(bridge.isReady()).toBe(false)
    await expect(bridge.waitUntilReady(20)).resolves.toBe(false)

    const socket = await new Promise<net.Socket>((resolve, reject) => {
      const client = net.createConnection({ host: '127.0.0.1', port }, () => {
        client.write(`${JSON.stringify({ token, kind: 'control_ready' })}\n`)
        resolve(client)
      })
      client.once('error', reject)
    })

    await expect(bridge.waitUntilReady(500)).resolves.toBe(true)
    expect(bridge.isReady()).toBe(true)

    socket.destroy()
    await bridge.close()
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

  it('sends model control commands with the selected model payload', async () => {
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
              text: '已切换模型：gpt-next'
            })}\n`
          )
        }
      })
      client.once('error', reject)
    })

    await new Promise((resolve) => setTimeout(resolve, 20))
    const result = await bridge.requestControlCommand(
      { command: 'model', model: 'gpt-next' },
      500
    )

    socket.destroy()
    await bridge.close()

    expect(result).toEqual({ ok: true, text: '已切换模型：gpt-next' })
    expect(receivedLines.join('')).toContain('"command":"model"')
    expect(receivedLines.join('')).toContain('"model":"gpt-next"')
    expect(receivedLines.join('')).toContain('"requestId"')
  })

  it('sends model reasoning control commands with the selected reasoning payload', async () => {
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
              text: '已切换推理档位：High'
            })}\n`
          )
        }
      })
      client.once('error', reject)
    })

    await new Promise((resolve) => setTimeout(resolve, 20))
    const result = await bridge.requestControlCommand(
      { command: 'model', reasoning: 'high' },
      500
    )

    socket.destroy()
    await bridge.close()

    expect(result).toEqual({ ok: true, text: '已切换推理档位：High' })
    expect(receivedLines.join('')).toContain('"command":"model"')
    expect(receivedLines.join('')).toContain('"reasoning":"high"')
    expect(receivedLines.join('')).toContain('"requestId"')
  })

  it('sends /btw control commands with the task payload', async () => {
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
              text: '已提交 /btw 子任务，完成后会通过 IM 回传。'
            })}\n`
          )
        }
      })
      client.once('error', reject)
    })

    await new Promise((resolve) => setTimeout(resolve, 20))
    const result = await bridge.requestControlCommand(
      { command: 'btw', task: '检查构建日志', replyId: 'reply-btw-fixed' },
      500
    )

    socket.destroy()
    await bridge.close()

    expect(result).toEqual({ ok: true, text: '已提交 /btw 子任务，完成后会通过 IM 回传。' })
    expect(receivedLines.join('')).toContain('"command":"btw"')
    expect(receivedLines.join('')).toContain('"task":"检查构建日志"')
    expect(receivedLines.join('')).toContain('"replyId":"reply-btw-fixed"')
    expect(receivedLines.join('')).toContain('"requestId"')
  })

  it('submits ordinary messages with separate model and TUI display text', async () => {
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
              text: 'queued'
            })}\n`
          )
        }
      })
      client.once('error', reject)
    })

    await new Promise((resolve) => setTimeout(resolve, 20))
    const result = await bridge.requestControlCommand(
      {
        command: 'submit_user_message',
        text: 'wrapped model prompt',
        displayText: '[来自远程 IM：phone]\n你好'
      },
      500
    )

    socket.destroy()
    await bridge.close()

    expect(result).toEqual({ ok: true, text: 'queued' })
    expect(receivedLines.join('')).toContain('"command":"submit_user_message"')
    expect(receivedLines.join('')).toContain('"text":"wrapped model prompt"')
    expect(receivedLines.join('')).toContain('"displayText":"[来自远程 IM：phone]\\n你好"')
  })

  it('sends lifecycle control commands without leaking them into normal text input', async () => {
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
        const command = String(chunk).match(/"command":"([^"]+)"/)?.[1]
        if (requestId) {
          client.write(
            `${JSON.stringify({
              token,
              kind: 'control_result',
              requestId,
              ok: true,
              text: `ok:${command}`
            })}\n`
          )
        }
      })
      client.once('error', reject)
    })

    await new Promise((resolve) => setTimeout(resolve, 20))
    const interrupt = await bridge.requestControlCommand({ command: 'interrupt' }, 500)
    const compact = await bridge.requestControlCommand({ command: 'compact' }, 500)
    const clear = await bridge.requestControlCommand({ command: 'clear' }, 500)

    socket.destroy()
    await bridge.close()

    expect(interrupt).toEqual({ ok: true, text: 'ok:interrupt' })
    expect(compact).toEqual({ ok: true, text: 'ok:compact' })
    expect(clear).toEqual({ ok: true, text: 'ok:clear' })
    expect(receivedLines.join('')).toContain('"command":"interrupt"')
    expect(receivedLines.join('')).toContain('"command":"compact"')
    expect(receivedLines.join('')).toContain('"command":"clear"')
    expect(receivedLines.join('')).not.toContain('"text":"/interrupt"')
  })
})
