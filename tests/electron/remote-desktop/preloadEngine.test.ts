import { describe, expect, it } from 'vitest'
import { resolveTrtcCloud, summarize } from '../../../electron/remote-desktop/preloadEngine.js'

class FakeTrtcCloud {
  static getTRTCShareInstance(): never {
    throw new Error('not called in this test')
  }
}

describe('resolveTrtcCloud', () => {
  it('digs the class out of the double default that Node gives a CJS package', () => {
    // 实测形状（Electron 33 + trtc-electron-sdk 13.3.801）：包是 CJS，
    // preload 是 ESM，Node 把整个 module.exports 塞进 mod.default，
    // 真正的类落在 mod.default.default。这一层就是线上那个
    // 「sdk.getTRTCShareInstance is not a function」的成因。
    const mod = {
      default: { default: FakeTrtcCloud, TRTCVideoResolution: {} },
      TRTCAppScene: {}
    }
    expect(resolveTrtcCloud(mod)).toBe(FakeTrtcCloud)
  })

  it('accepts the shape a bundler produces when it honours __esModule', () => {
    expect(resolveTrtcCloud({ default: FakeTrtcCloud })).toBe(FakeTrtcCloud)
  })

  it('accepts a plain CJS require result', () => {
    expect(resolveTrtcCloud(FakeTrtcCloud)).toBe(FakeTrtcCloud)
  })

  it('prefers the deepest candidate that actually exposes the static', () => {
    // mod.default 上没有这个方法时不能就此收手——外层看着像模块，
    // 但能用的是里层那个。挑选必须按「谁有这个方法」，不是按层级先后。
    const outerWithoutStatic = { default: { default: FakeTrtcCloud } }
    expect(resolveTrtcCloud(outerWithoutStatic)).toBe(FakeTrtcCloud)
  })

  it('summarizes log payloads without leaking credentials', () => {
    // 排障日志会被贴进聊天窗口发给我看，凭证绝不能跟着出去。
    const summarized = summarize({
      userSig: 'eJyrVgrxCdZLrSjILEpVsl',
      secretKey: 'abc',
      apiToken: 'xyz',
      roomId: 'room-42',
      sdkAppId: 1400000000
    }) as Record<string, unknown>

    expect(summarized.userSig).toBe('[redacted:22]')
    expect(summarized.secretKey).toBe('[redacted:3]')
    expect(summarized.apiToken).toBe('[redacted:3]')
    // 房间号和 appId 必须留着，双机对不上房间时全靠它们对账。
    expect(summarized.roomId).toBe('room-42')
    expect(summarized.sdkAppId).toBe(1400000000)
  })

  it('keeps log payloads small', () => {
    // TRTC 事件可能带庞大的原生对象，原样写进日志会把日志冲垮。
    expect(summarize('x'.repeat(500))).toHaveLength(201)
    expect(summarize([1, 2, 3])).toBe('[array:3]')
    expect((summarize({ nested: { deep: 1 } }) as Record<string, unknown>).nested).toBe('[object]')
  })

  it('fails loudly instead of returning something unusable', () => {
    // 静默返回一个没有该方法的对象，错误会推迟到 getTRTCShareInstance() 调用处，
    // 报出来的信息就跟这次线上一样含糊。
    for (const bad of [null, undefined, {}, { default: {} }, { default: { default: {} } }, 42]) {
      expect(() => resolveTrtcCloud(bad), JSON.stringify(bad) ?? String(bad)).toThrow(
        'getTRTCShareInstance'
      )
    }
  })
})
