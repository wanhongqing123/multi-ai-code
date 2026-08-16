import { describe, expect, it } from 'vitest'
import {
  SCREEN_CAPTURE_SOURCE_TYPE_SCREEN,
  TRTC_VIDEO_STREAM_TYPE_SUB,
  resolveTrtcCloud,
  resolveSdkClass,
  summarize
} from '../../../electron/remote-desktop/preloadEngine.js'

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

class FakeSdkClass {}

describe('resolveSdkClass', () => {
  // SDK 到处用 instanceof 校验入参，传对象字面量既不抛错也不返回失败，
  // 只往它自己的 logger 写一行——原生调用根本没发生。已经咬过三次：
  //   enterRoom                 → 静默不进房，表现为「进房超时」
  //   selectScreenCaptureTarget → 静默不选源，表现为对端「已连接但黑屏」
  // 实测：换成真实例后 137ms 进房成功。
  it('finds a class across every interop shape', () => {
    expect(resolveSdkClass({ Rect: FakeSdkClass }, 'Rect')).toBe(FakeSdkClass)
    expect(resolveSdkClass({ default: { Rect: FakeSdkClass } }, 'Rect')).toBe(FakeSdkClass)
    expect(resolveSdkClass({ default: { default: { Rect: FakeSdkClass } } }, 'Rect')).toBe(
      FakeSdkClass
    )
  })

  it('resolves every class the engine constructs', () => {
    // 少任何一个都会让对应的调用被静默丢弃，所以四个都要能取到。
    const mod = {
      default: {
        default: {
          TRTCParams: FakeSdkClass,
          Rect: FakeSdkClass,
          TRTCScreenCaptureProperty: FakeSdkClass,
          TRTCVideoEncParam: FakeSdkClass
        }
      }
    }
    for (const name of ['TRTCParams', 'Rect', 'TRTCScreenCaptureProperty', 'TRTCVideoEncParam']) {
      expect(resolveSdkClass(mod, name), name).toBe(FakeSdkClass)
    }
  })

  it('throws rather than let the SDK silently drop the call', () => {
    // 取不到就当场炸。退回对象字面量的话，调用会被静默丢弃，
    // 而这个故障模式极难查——正是前面绕了三大圈的原因。
    for (const bad of [null, undefined, {}, { default: {} }, { Rect: 'nope' }]) {
      expect(() => resolveSdkClass(bad, 'Rect'), String(bad)).toThrow('Rect')
    }
  })
})

describe('screen capture source type', () => {
  it('uses Screen (1), not Window (0)', () => {
    // TRTCScreenCaptureSourceType: Unknown=-1, Window=0, Screen=1, Custom=2。
    // 写成 0 时过滤出来的全是窗口，"整屏共享"实际共享的是列表里的第一个窗口，
    // 对端看到一个应用窗口加一圈黑边——线上就是这么表现的。
    expect(SCREEN_CAPTURE_SOURCE_TYPE_SCREEN).toBe(1)
  })
})

describe('screen share stream type', () => {
  it('uses Sub (2), not Small (1)', () => {
    // TRTCVideoStreamType: Big=0, Small=1, Sub=2。曾经写成 1 还能跑纯属侥幸——
    // startScreenCapture 有「非 Sub/Big 一律纠正为 Sub」的兜底，把错值悄悄改对了。
    // 但统计和事件回报的 streamType 是真实的 2，拿 1 去比对就永远匹配不上，
    // 排障时会看到「一帧都没有」的假象。
    expect(TRTC_VIDEO_STREAM_TYPE_SUB).toBe(2)
  })
})

describe('remote desktop log payloads', () => {
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

})
