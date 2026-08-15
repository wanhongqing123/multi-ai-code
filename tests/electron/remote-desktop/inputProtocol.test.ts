import { describe, expect, it } from 'vitest'
import {
  REMOTE_INPUT_CMD_ID_RELIABLE,
  REMOTE_INPUT_CMD_ID_UNRELIABLE,
  REMOTE_INPUT_PROTOCOL_VERSION,
  decodeRemoteInputPacket
} from '../../../electron/remote-desktop/inputProtocol.js'

function packet(events: unknown[], overrides: Record<string, unknown> = {}): string {
  return JSON.stringify({ v: REMOTE_INPUT_PROTOCOL_VERSION, s: 'sess-1', n: 7, e: events, ...overrides })
}

describe('remote input protocol', () => {
  it('keeps the cmd ids MaiChat sends on', () => {
    // 两条通道的丢包语义相反（移动丢了自愈，按键丢了留下悬空状态），
    // cmdID 对不上就会用错兜底策略，所以钉死。
    expect(REMOTE_INPUT_CMD_ID_UNRELIABLE).toBe(2)
    expect(REMOTE_INPUT_CMD_ID_RELIABLE).toBe(3)
  })

  it('decodes every event type MaiChat can send', () => {
    const decoded = decodeRemoteInputPacket(
      packet([
        { t: 'm', x: 0.25, y: 0.5 },
        { t: 'b', x: 0.1, y: 0.2, b: 1, d: true },
        { t: 'w', x: 0.3, y: 0.4, w: -120 },
        { t: 'k', k: 65, d: true },
        { t: 'x', s: '中文输入' },
        { t: 'r' }
      ])
    )

    expect(decoded?.sessionId).toBe('sess-1')
    expect(decoded?.sequence).toBe(7)
    expect(decoded?.events).toEqual([
      { type: 'mouseMove', x: 0.25, y: 0.5 },
      { type: 'mouseButton', x: 0.1, y: 0.2, button: 'right', pressed: true },
      { type: 'mouseWheel', x: 0.3, y: 0.4, wheelDelta: -120 },
      { type: 'key', keyCode: 65, pressed: true },
      { type: 'text', text: '中文输入' },
      { type: 'releaseAll' }
    ])
  })

  it('drops the whole packet when the envelope is untrustworthy', () => {
    // 宁可不动，也不要用错误的语义去操作别人的电脑。
    const bad = [
      'not json',
      '[1,2,3]',
      '"string"',
      JSON.stringify({ v: 99, s: 's', n: 1, e: [] }),
      JSON.stringify({ v: 1, s: 's', n: -1, e: [] }),
      JSON.stringify({ v: 1, s: 's', n: 0x100000000, e: [] }),
      JSON.stringify({ v: 1, s: 's', n: 1.5, e: [] }),
      JSON.stringify({ v: 1, s: 's', n: 1, e: 'nope' })
    ]
    for (const payload of bad) {
      expect(decodeRemoteInputPacket(payload), payload).toBeNull()
    }
  })

  it('skips only the bad event instead of failing the packet', () => {
    // 未知事件多半来自新版主控端；整包丢弃会让一个新事件卡死整条输入流。
    const decoded = decodeRemoteInputPacket(
      packet([
        { t: 'm', x: 0.5, y: 0.5 },
        { t: 'zzz' },
        { t: 'k', k: 70000, d: true },
        { t: 'k', k: -1, d: true },
        { t: 'x', s: '' },
        'not-an-object',
        { t: 'b', x: 0.9, y: 0.9, b: 0, d: false }
      ])
    )

    expect(decoded?.events).toEqual([
      { type: 'mouseMove', x: 0.5, y: 0.5 },
      { type: 'mouseButton', x: 0.9, y: 0.9, button: 'left', pressed: false }
    ])
  })

  it('clamps coordinates and blocks NaN from reaching the injector', () => {
    // NaN 比不出大小，min/max 两个分支都不命中——不显式挡掉就会一路传到注入层，
    // 把光标打到未定义的位置。
    const decoded = decodeRemoteInputPacket(
      packet([
        { t: 'm', x: -3, y: 42 },
        { t: 'm', x: 'nope', y: null },
        { t: 'm', x: 0.5 }
      ])
    )

    expect(decoded?.events).toEqual([
      { type: 'mouseMove', x: 0, y: 1 },
      { type: 'mouseMove', x: 0, y: 0 },
      { type: 'mouseMove', x: 0.5, y: 0 }
    ])
  })

  it('falls back to the left button for unknown button ids', () => {
    // 宁可点错一个键，也不要当成"没有按键"而漏掉配对的抬起——那会留下悬空状态。
    const decoded = decodeRemoteInputPacket(packet([{ t: 'b', x: 0, y: 0, b: 99, d: true }]))
    expect(decoded?.events).toEqual([
      { type: 'mouseButton', x: 0, y: 0, button: 'left', pressed: true }
    ])
  })

  it('treats a missing pressed flag as released', () => {
    const decoded = decodeRemoteInputPacket(packet([{ t: 'k', k: 13 }]))
    expect(decoded?.events).toEqual([{ type: 'key', keyCode: 13, pressed: false }])
  })
})
