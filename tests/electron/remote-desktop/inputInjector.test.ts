import { describe, expect, it } from 'vitest'
import {
  REMOTE_INPUT_SILENCE_TIMEOUT_MS,
  createRemoteInputInjector,
  isBlockedKeyCombination,
  type RemoteInputSink
} from '../../../electron/remote-desktop/inputInjector.js'
import type { RemoteInputEvent, RemoteInputPacket } from '../../../electron/remote-desktop/inputProtocol.js'

function fakeSink() {
  const calls: string[] = []
  const sink: RemoteInputSink = {
    moveTo: (x, y) => calls.push(`move:${x},${y}`),
    mouseButton: (button, pressed, x, y) =>
      calls.push(`button:${button}:${pressed ? 'down' : 'up'}@${x},${y}`),
    wheel: (delta, x, y) => calls.push(`wheel:${delta}@${x},${y}`),
    key: (code, pressed) => calls.push(`key:${code}:${pressed ? 'down' : 'up'}`),
    text: (value) => calls.push(`text:${value}`)
  }
  return { sink, calls }
}

function packet(events: RemoteInputEvent[], overrides: Partial<RemoteInputPacket> = {}) {
  return {
    protocolVersion: 1,
    sessionId: 'session-1',
    sequence: 1,
    events,
    ...overrides
  }
}

const VK_LEFT_WIN = 0x5b
const VK_L = 0x4c

describe('remote input injector', () => {
  it('refuses every packet before a session begins', () => {
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)

    expect(injector.handlePacket(packet([{ type: 'mouseMove', x: 0.5, y: 0.5 }]), 'unreliable', 0))
      .toBe(false)
    expect(calls).toEqual([])
  })

  it('refuses packets carrying a different session id', () => {
    // 上一场会话的残留包不该操作这一场的电脑。
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')

    expect(
      injector.handlePacket(
        packet([{ type: 'mouseMove', x: 0.5, y: 0.5 }], { sessionId: 'session-old' }),
        'unreliable',
        0
      )
    ).toBe(false)
    expect(calls).toEqual([])
  })

  it('drops out-of-order moves instead of jumping the cursor backwards', () => {
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')

    injector.handlePacket(packet([{ type: 'mouseMove', x: 0.9, y: 0.9 }], { sequence: 5 }), 'unreliable', 0)
    // 迟到的旧包：位置更旧，盖上去光标会来回跳。
    expect(
      injector.handlePacket(packet([{ type: 'mouseMove', x: 0.1, y: 0.1 }], { sequence: 4 }), 'unreliable', 0)
    ).toBe(false)

    expect(calls).toEqual(['move:0.9,0.9'])
  })

  it('releases everything held when the reliable channel skips sequences', () => {
    // 可靠有序通道跳号说明链路出了问题，中间那些抬起包多半丢了。
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')

    injector.handlePacket(packet([{ type: 'key', keyCode: 0x11, pressed: true }], { sequence: 1 }), 'reliable', 0)
    expect(injector.heldKeys()).toEqual([0x11])

    injector.handlePacket(packet([{ type: 'key', keyCode: 0x41, pressed: true }], { sequence: 9 }), 'reliable', 0)

    expect(calls).toEqual(['key:17:down', 'key:17:up', 'key:65:down'])
    expect(injector.heldKeys()).toEqual([0x41])
  })

  it('does not release on the gap the protocol tolerates', () => {
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')

    injector.handlePacket(packet([{ type: 'key', keyCode: 0x11, pressed: true }], { sequence: 1 }), 'reliable', 0)
    injector.handlePacket(packet([{ type: 'key', keyCode: 0x41, pressed: true }], { sequence: 2 }), 'reliable', 0)

    expect(calls).toEqual(['key:17:down', 'key:65:down'])
  })

  it('releases held input when the session ends', () => {
    // 控制端崩了、网断了，被控端都不该留着 Ctrl 按住——人不在电脑旁，没法自己解。
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')
    injector.handlePacket(
      packet([
        { type: 'key', keyCode: 0x11, pressed: true },
        { type: 'mouseButton', x: 0.5, y: 0.5, button: 'left', pressed: true }
      ]),
      'reliable',
      0
    )
    calls.length = 0

    injector.endSession()

    expect(calls).toEqual(['key:17:up', 'button:left:up@0,0'])
    expect(injector.hasAnythingHeld()).toBe(false)
  })

  it('cleans up leftovers from a crashed session when a new one begins', () => {
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')
    injector.handlePacket(packet([{ type: 'key', keyCode: 0x11, pressed: true }]), 'reliable', 0)
    calls.length = 0

    // 没有 endSession 就直接开新会话：模拟上一场异常结束。
    injector.beginSession('session-2')

    expect(calls).toEqual(['key:17:up'])
    expect(injector.hasAnythingHeld()).toBe(false)
  })

  it('releases held input after the watchdog silence timeout', () => {
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')
    injector.handlePacket(packet([{ type: 'key', keyCode: 0x11, pressed: true }]), 'reliable', 1000)
    calls.length = 0

    injector.tickWatchdog(1000 + REMOTE_INPUT_SILENCE_TIMEOUT_MS - 1)
    expect(calls).toEqual([])

    injector.tickWatchdog(1000 + REMOTE_INPUT_SILENCE_TIMEOUT_MS)
    expect(calls).toEqual(['key:17:up'])
  })

  it('does not fire the watchdog before any packet arrives', () => {
    // lastPacketMs 为 0 说明这一场还没收到输入，谈不上"静默"。
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')

    injector.tickWatchdog(999_999)

    expect(calls).toEqual([])
  })

  it('blocks Win+L and swallows its release too', () => {
    // 锁屏是不可逆的单向门：屏幕共享还在跑但画面是锁屏界面，
    // 而解锁需要人站在那台电脑前。
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')

    injector.handlePacket(packet([{ type: 'key', keyCode: VK_LEFT_WIN, pressed: true }], { sequence: 1 }), 'reliable', 0)
    injector.handlePacket(packet([{ type: 'key', keyCode: VK_L, pressed: true }], { sequence: 2 }), 'reliable', 0)
    injector.handlePacket(packet([{ type: 'key', keyCode: VK_L, pressed: false }], { sequence: 3 }), 'reliable', 0)

    // 只有 Win 键落地；L 的按下和抬起都被吞掉——只拦按下会让控制端
    // 以为键还按着，而本地根本没按下去。
    expect(calls).toEqual(['key:91:down'])
    expect(injector.heldKeys()).toEqual([VK_LEFT_WIN])
  })

  it('allows a bare L when no Win key is held', () => {
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')

    injector.handlePacket(packet([{ type: 'key', keyCode: VK_L, pressed: true }]), 'reliable', 0)

    expect(calls).toEqual(['key:76:down'])
    expect(isBlockedKeyCombination(VK_L, new Set())).toBe(false)
    expect(isBlockedKeyCombination(VK_L, new Set([VK_LEFT_WIN]))).toBe(true)
  })

  it('honours an explicit releaseAll event', () => {
    const { sink, calls } = fakeSink()
    const injector = createRemoteInputInjector(sink)
    injector.beginSession('session-1')
    injector.handlePacket(
      packet([
        { type: 'key', keyCode: 0x11, pressed: true },
        { type: 'releaseAll' }
      ]),
      'reliable',
      0
    )

    expect(calls).toEqual(['key:17:down', 'key:17:up'])
    expect(injector.hasAnythingHeld()).toBe(false)
  })
})
