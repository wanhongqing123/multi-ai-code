import { describe, expect, it } from 'vitest'
import {
  REMOTE_INPUT_CMD_ID_RELIABLE,
  REMOTE_INPUT_CMD_ID_UNRELIABLE,
  channelForCmdId,
  clampNormalized,
  decodeRemoteInputPacket
} from '../../../electron/remote-desktop/inputProtocol.js'

/** 按 MaiChat 的线格式拼一个包，便于逐字段对齐。 */
function encode(events: unknown[], overrides: Record<string, unknown> = {}): string {
  return JSON.stringify({ v: 1, s: 'session-1', n: 1, e: events, ...overrides })
}

describe('remote input wire format', () => {
  it('decodes every event type MaiChat can send', () => {
    const packet = decodeRemoteInputPacket(
      encode([
        { t: 'm', x: 0.25, y: 0.75 },
        { t: 'b', x: 0.5, y: 0.5, b: 1, d: true },
        { t: 'w', x: 0.1, y: 0.2, w: -240 },
        { t: 'k', k: 0x41, d: true },
        { t: 'x', s: '你好' },
        { t: 'r' }
      ])
    )

    expect(packet?.sessionId).toBe('session-1')
    expect(packet?.sequence).toBe(1)
    expect(packet?.events).toEqual([
      { type: 'mouseMove', x: 0.25, y: 0.75 },
      { type: 'mouseButton', x: 0.5, y: 0.5, button: 'right', pressed: true },
      { type: 'mouseWheel', x: 0.1, y: 0.2, wheelDelta: -240 },
      { type: 'key', keyCode: 0x41, pressed: true },
      { type: 'text', text: '你好' },
      { type: 'releaseAll' }
    ])
  })

  it('maps the two cmd ids to their reliability channels', () => {
    // 同一 cmdID 内可靠性必须前后一致（TRTC 硬约束），所以协议拆成两个 ID。
    // 认错通道会让鼠标移动走可靠有序、把按键挤掉。
    expect(channelForCmdId(REMOTE_INPUT_CMD_ID_UNRELIABLE)).toBe('unreliable')
    expect(channelForCmdId(REMOTE_INPUT_CMD_ID_RELIABLE)).toBe('reliable')
    expect(channelForCmdId(1)).toBeNull()
    expect(channelForCmdId(99)).toBeNull()
  })

  it('rejects a whole packet whose protocol version differs', () => {
    // 宁可不动，也不要用错误的语义去操作别人的电脑。
    expect(decodeRemoteInputPacket(encode([{ t: 'r' }], { v: 2 }))).toBeNull()
    expect(decodeRemoteInputPacket(encode([{ t: 'r' }], { v: '1' }))).toBeNull()
  })

  it('rejects malformed packets instead of half-parsing them', () => {
    for (const bad of [
      '',
      'not json',
      '[]',
      'null',
      JSON.stringify({ v: 1, s: 'x', n: 1 }), // 缺 e
      JSON.stringify({ v: 1, s: 'x', n: 1, e: 'nope' }), // e 不是数组
      JSON.stringify({ v: 1, s: 'x', e: [] }), // 缺 n
      JSON.stringify({ v: 1, s: 'x', n: -1, e: [] }), // 序号为负
      JSON.stringify({ v: 1, s: 'x', n: 0x100000000, e: [] }) // 序号越界
    ]) {
      expect(decodeRemoteInputPacket(bad), bad.slice(0, 40)).toBeNull()
    }
  })

  it('skips a bad event without discarding the good ones in the same packet', () => {
    // 一条坏事件不该让整包报废——那会把同包里合法的抬起也丢掉。
    const packet = decodeRemoteInputPacket(
      encode([{ t: 'm', x: 0.5, y: 0.5 }, { t: '?' }, 'garbage', null, { t: 'r' }])
    )

    expect(packet?.events.map((e) => e.type)).toEqual(['mouseMove', 'releaseAll'])
  })

  it('drops key events whose code could not be a real virtual key', () => {
    // 越界键码拿去 SendInput 会点到完全无关的键。
    for (const code of [-1, 0x10000, 999999]) {
      const packet = decodeRemoteInputPacket(encode([{ t: 'k', k: code, d: true }]))
      expect(packet?.events, String(code)).toEqual([])
    }
    // 边界值本身是合法的。
    expect(decodeRemoteInputPacket(encode([{ t: 'k', k: 0xffff, d: false }]))?.events).toEqual([
      { type: 'key', keyCode: 0xffff, pressed: false }
    ])
  })

  it('drops empty text events', () => {
    expect(decodeRemoteInputPacket(encode([{ t: 'x', s: '' }]))?.events).toEqual([])
    expect(decodeRemoteInputPacket(encode([{ t: 'x' }]))?.events).toEqual([])
  })

  it('clamps coordinates so a bad packet cannot fling the cursor off screen', () => {
    const packet = decodeRemoteInputPacket(
      encode([
        { t: 'm', x: -5, y: 12 },
        { t: 'm', x: 'nope', y: null }
      ])
    )

    expect(packet?.events).toEqual([
      { type: 'mouseMove', x: 0, y: 1 },
      { type: 'mouseMove', x: 0, y: 0 }
    ])
    expect(clampNormalized(Number.NaN)).toBe(0)
  })

  it('falls back to the left button for an unknown button index', () => {
    // 宁可点错一个键，也不要把它当成"没有按键"而漏掉配对的抬起。
    const packet = decodeRemoteInputPacket(encode([{ t: 'b', x: 0, y: 0, b: 77, d: false }]))
    expect(packet?.events).toEqual([
      { type: 'mouseButton', x: 0, y: 0, button: 'left', pressed: false }
    ])
  })

  it('treats a non-boolean pressed flag as released', () => {
    // 与 Qt 的 toBool() 对齐。读成"按下"会留下永远抬不起来的键。
    const packet = decodeRemoteInputPacket(encode([{ t: 'k', k: 65, d: 'true' }]))
    expect(packet?.events).toEqual([{ type: 'key', keyCode: 65, pressed: false }])
  })
})
