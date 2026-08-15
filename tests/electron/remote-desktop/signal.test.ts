import { describe, expect, it } from 'vitest'
import {
  REMOTE_DESKTOP_NOTICE_CODES,
  REMOTE_DESKTOP_PROTOCOL_VERSION,
  REMOTE_DESKTOP_SIGNAL_PREFIX,
  decodeRemoteDesktopSignal,
  encodeRemoteDesktopSignal,
  isRemoteDesktopSignalText,
  type RemoteDesktopCaptureGeometry
} from '../../../electron/remote-desktop/signal.js'

const geometry: RemoteDesktopCaptureGeometry = {
  sourceWidth: 2560,
  sourceHeight: 1600,
  captureX: 0,
  captureY: 0,
  captureWidth: 2560,
  captureHeight: 1600,
  contentMode: 'fit',
  revision: 1
}

describe('remote desktop signal', () => {
  it('uses the exact invisible prefix MaiChat looks for', () => {
    // MaiChat 判定信令只看这个前缀（RemoteDesktopSignal.cpp 的 kInvisiblePrefix + kMarker）。
    // 差一个码点，两端就完全认不出对方，而且不会有任何报错——所以逐码点钉死。
    const points = Array.from(REMOTE_DESKTOP_SIGNAL_PREFIX)
      .slice(0, 2)
      .map((char) => char.codePointAt(0))
    expect(points).toEqual([0x2063, 0x200b])
    expect(REMOTE_DESKTOP_SIGNAL_PREFIX.endsWith('[remote-desktop]')).toBe(true)
  })

  it('rejects the same non-signal texts MaiChat rejects', () => {
    // 这批取自 MaiChat 自己的 RemoteDesktopSignalTest.cpp：同样的输入两端必须同样判定，
    // 否则"某一端把普通聊天当信令"这种错会很难查。
    const texts = [
      '你好',
      '[remote-desktop] 看起来像信令但没有前缀',
      '',
      '{"v":1,"type":"invite"}'
    ]
    for (const text of texts) {
      expect(isRemoteDesktopSignalText(text), text).toBe(false)
      expect(decodeRemoteDesktopSignal(text), text).toBeNull()
    }
  })

  it('rejects malformed payloads behind a valid prefix', () => {
    const payloads = [
      'not json at all',
      '{',
      '[1,2,3]',
      '"just-a-string"',
      '{"v":1}',
      '{"v":99,"type":"invite","sessionId":"s"}',
      '{"v":1,"type":"shutdown","sessionId":"s"}'
    ]
    for (const payload of payloads) {
      const text = REMOTE_DESKTOP_SIGNAL_PREFIX + payload
      // 前缀是对的，所以它确实"看起来像信令"——但内容不合法时必须当普通消息处理。
      expect(isRemoteDesktopSignalText(text), payload).toBe(true)
      expect(decodeRemoteDesktopSignal(text), payload).toBeNull()
    }
  })

  it('decodes an invite produced by MaiChat', () => {
    // 被控端只会收到 invite / stop；这条是主控端发起时的真实形态。
    const text =
      REMOTE_DESKTOP_SIGNAL_PREFIX +
      JSON.stringify({
        v: REMOTE_DESKTOP_PROTOCOL_VERSION,
        type: 'invite',
        sessionId: 's-777',
        roomId: 'mc-a-b',
        authProof: '9f86d081884c7d659a2feaa0c55ad015'
      })

    expect(decodeRemoteDesktopSignal(text)).toEqual({
      type: 'invite',
      sessionId: 's-777',
      roomId: 'mc-a-b',
      authProof: '9f86d081884c7d659a2feaa0c55ad015'
    })
  })

  it('round-trips an accept carrying capture geometry', () => {
    const encoded = encodeRemoteDesktopSignal({
      type: 'accept',
      sessionId: 's-1',
      roomId: 'mc-a-b',
      captureGeometry: geometry
    })

    expect(isRemoteDesktopSignalText(encoded)).toBe(true)
    expect(decodeRemoteDesktopSignal(encoded)).toEqual({
      type: 'accept',
      sessionId: 's-1',
      roomId: 'mc-a-b',
      captureGeometry: geometry
    })
  })

  it('keeps a valid accept when only the geometry is broken', () => {
    // captureGeometry 是 v1 的兼容扩展：坏掉只该丢掉坐标增强，
    // 把整条 accept 打成非法会让新旧版本混连时进不了房。
    const broken = [
      { ...geometry, captureWidth: 0 },
      { ...geometry, captureX: 2560 },
      { ...geometry, revision: 0 },
      { ...geometry, contentMode: 'fill' },
      { ...geometry, sourceWidth: 70000 },
      { ...geometry, sourceHeight: 1600.5 }
    ]
    for (const captureGeometry of broken) {
      const text =
        REMOTE_DESKTOP_SIGNAL_PREFIX +
        JSON.stringify({
          v: REMOTE_DESKTOP_PROTOCOL_VERSION,
          type: 'accept',
          sessionId: 's-1',
          captureGeometry
        })
      const decoded = decodeRemoteDesktopSignal(text)
      expect(decoded?.type, JSON.stringify(captureGeometry)).toBe('accept')
      expect(decoded?.captureGeometry, JSON.stringify(captureGeometry)).toBeUndefined()
    }
  })

  it('drops a notice without a code', () => {
    // 没带 code 的 notice 对端不知道该显示什么，等于一条空播报。
    const text =
      REMOTE_DESKTOP_SIGNAL_PREFIX +
      JSON.stringify({ v: REMOTE_DESKTOP_PROTOCOL_VERSION, type: 'notice', sessionId: 's-1' })
    expect(decodeRemoteDesktopSignal(text)).toBeNull()

    const withCode = encodeRemoteDesktopSignal({
      type: 'notice',
      sessionId: 's-1',
      noticeCode: REMOTE_DESKTOP_NOTICE_CODES.secureDesktopEntered
    })
    expect(decodeRemoteDesktopSignal(withCode)?.noticeCode).toBe('secure-desktop-entered')
  })

  it('omits empty fields and never attaches geometry to non-accept signals', () => {
    const stop = encodeRemoteDesktopSignal({
      type: 'stop',
      sessionId: 's-1',
      reason: '',
      captureGeometry: geometry
    })
    const payload = JSON.parse(stop.slice(REMOTE_DESKTOP_SIGNAL_PREFIX.length))

    expect(payload).toEqual({ v: 1, type: 'stop', sessionId: 's-1' })
    expect(payload.reason).toBeUndefined()
    expect(payload.captureGeometry).toBeUndefined()
  })
})
