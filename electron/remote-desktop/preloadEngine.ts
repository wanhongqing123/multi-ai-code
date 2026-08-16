// TRTC 引擎的真实现，跑在 preload。
//
// 为什么是 preload 而不是渲染进程或主进程——三个约束交叉之后只剩这一个位置：
//   主进程     ：SDK 的 index.js re-export 了模块级访问 DOM 的模块，
//                require 直接抛 "document is not defined"（实测）
//   渲染进程   ：应用是 nodeIntegration:false + contextIsolation:true，
//                拿不到 require，而且打包器会试图把 .node 二进制打进 bundle
//   preload    ：既有 Node（可 require 原生模块，且 externalizeDepsPlugin 会
//                把它排除出 bundle），又有 DOM。
//
// 判定逻辑一概不在这里：本文件只把接口调用翻译成 SDK 调用。

import type {
  RemoteDesktopCaptureSource,
  RemoteDesktopEnterRoomParams,
  RemoteDesktopEngine
} from './engine.js'

/** TRTCVideoStreamType.Sub：屏幕共享走辅路。 */
const TRTC_VIDEO_STREAM_TYPE_SUB = 1
/** TRTCAppScene.VideoCall：不区分角色，sendCustomCmdMsg 天然可用。 */
const TRTC_APP_SCENE_VIDEO_CALL = 0
/** TRTCScreenCaptureSourceType.Screen：整屏，不是窗口。 */
const SCREEN_CAPTURE_SOURCE_TYPE_SCREEN = 0
const ENTER_ROOM_TIMEOUT_MS = 15000

/**
 * 进房等待期间旁听的 TRTC 事件。
 *
 * 只监听 onEnterRoom / onError 的话，两个都不来就只剩一句「进房超时」，
 * 没有任何线索——第一次线上失败就是这样，只能靠猜。这些事件本身不改变
 * 判定，纯粹是为了让超时报告能说出「这 15 秒里到底来了什么」。
 */
const OBSERVED_ENTER_ROOM_EVENTS = [
  'onError',
  'onWarning',
  'onEnterRoom',
  'onExitRoom',
  'onConnectionLost',
  'onTryToReconnect',
  'onConnectionRecovery',
  'onSwitchRole',
  'onFirstVideoFrame'
] as const

interface TrtcSdkSource {
  sourceId: string
  sourceName: string
  type: number
  isMainScreen?: boolean
}

interface TrtcInstance {
  on(event: string, handler: (...args: unknown[]) => void): void
  off?(event: string, handler: (...args: unknown[]) => void): void
  enterRoom(params: Record<string, unknown>, scene: number): void
  exitRoom(): void
  getScreenCaptureSources(
    thumbWidth: number,
    thumbHeight: number,
    iconWidth: number,
    iconHeight: number
  ): TrtcSdkSource[]
  selectScreenCaptureTarget(
    source: TrtcSdkSource,
    rect: { left: number; top: number; right: number; bottom: number },
    options: Record<string, unknown>
  ): void
  startScreenCapture(view: unknown, streamType: number, params: unknown): void
  stopScreenCapture(): void
}

interface TrtcCloudClass {
  getTRTCShareInstance(): TrtcInstance
}

/**
 * 从动态 import 的结果里挑出 TRTCCloud 类。
 *
 * 包是 CJS（`exports.default = TRTCCloud`），而 preload 打出来是 ESM，
 * 于是 Node 把整个 `module.exports` 塞进 `mod.default`——真正的类落在
 * `mod.default.default`。直接取 `mod.default` 拿到的是导出对象，调用时报
 * 「sdk.getTRTCShareInstance is not a function」。
 *
 * 三种形状都试，是因为这层 interop 取决于谁来打包：Node 原生 ESM 加载器、
 * 尊重 __esModule 的打包器、还是直接 require，结果各不相同。与其赌一种，
 * 不如认「哪个上面有这个方法就用哪个」。
 */
export function resolveTrtcCloud(mod: unknown): TrtcCloudClass {
  const holder = mod as {
    default?: { default?: unknown } | unknown
  }
  const candidates = [
    (holder as { default?: { default?: unknown } })?.default?.default,
    holder?.default,
    mod
  ]
  for (const candidate of candidates) {
    if (typeof (candidate as TrtcCloudClass | undefined)?.getTRTCShareInstance === 'function') {
      return candidate as TrtcCloudClass
    }
  }
  throw new Error('TRTC SDK 加载异常：找不到 getTRTCShareInstance')
}

interface TrtcParamsClass {
  new (): Record<string, unknown>
}

/**
 * 从 SDK 模块里取出 TRTCParams 构造函数。
 *
 * 为什么非得用它、不能传对象字面量——trtc.js 的 enterRoom 第一行是：
 *
 *   if (params instanceof TRTCParams) { this.rtcCloud.enterRoom(...) }
 *   else { this.logger.error('params is not instanceof TRTCParams!') }
 *
 * 传普通对象不会抛错、不会返回失败，只往它自己的 logger 写一行就结束了。
 * 原生调用根本没发生，于是 onEnterRoom / onError 一个都不会来，表现就是
 * 「进房超时」——查了半天以为是凭证、代理、上下文的问题，其实调用压根没出去。
 *
 * 与 resolveTrtcCloud 同理，三种 interop 形状都试。
 */
export function resolveTrtcParams(mod: unknown): TrtcParamsClass {
  const holder = mod as { default?: { default?: unknown; TRTCParams?: unknown }; TRTCParams?: unknown }
  const candidates = [
    holder?.TRTCParams,
    holder?.default?.TRTCParams,
    (holder?.default?.default as { TRTCParams?: unknown } | undefined)?.TRTCParams
  ]
  for (const candidate of candidates) {
    if (typeof candidate === 'function') return candidate as TrtcParamsClass
  }
  throw new Error('TRTC SDK 加载异常：找不到 TRTCParams')
}

export type RemoteDesktopEngineLogger = (
  message: string,
  detail?: Record<string, unknown>
) => void

/**
 * TRTC 事件的参数原样塞进日志会有两个问题：可能带凭证，也可能是庞大的
 * 原生对象。这里只留够定位问题的部分。
 */
export function summarize(value: unknown): unknown {
  if (value === null || value === undefined) return value
  if (typeof value === 'string') return value.length > 200 ? `${value.slice(0, 200)}…` : value
  if (typeof value === 'number' || typeof value === 'boolean') return value
  if (Array.isArray(value)) return `[array:${value.length}]`
  if (typeof value === 'object') {
    const out: Record<string, unknown> = {}
    for (const [key, item] of Object.entries(value as Record<string, unknown>)) {
      if (/sig|token|secret|key/i.test(key)) {
        out[key] = typeof item === 'string' ? `[redacted:${item.length}]` : '[redacted]'
        continue
      }
      out[key] = typeof item === 'object' && item !== null ? '[object]' : item
    }
    return out
  }
  return String(value)
}

export function createTrtcRemoteDesktopEngine(
  logger?: RemoteDesktopEngineLogger
): RemoteDesktopEngine {
  let instance: TrtcInstance | null = null
  let trtcParamsClass: TrtcParamsClass | null = null
  let sharing = false
  const log = (message: string, detail?: Record<string, unknown>): void => {
    try {
      logger?.(`remote-desktop-engine: ${message}`, detail)
    } catch {
      // 记日志本身失败绝不能连累共享流程。
    }
  }

  async function ensureInstance(): Promise<TrtcInstance> {
    if (instance) return instance
    // 动态 import 而不是顶层 import：SDK 带 29MB 原生二进制，没开远程桌面的
    // 用户不该在每次启动时都为它付加载成本。
    const mod: unknown = await import('trtc-electron-sdk')
    const cloud = resolveTrtcCloud(mod)
    trtcParamsClass = resolveTrtcParams(mod)
    instance = cloud.getTRTCShareInstance()
    log('sdk ready')
    return instance
  }

  function listRawSources(trtc: TrtcInstance): TrtcSdkSource[] {
    return (trtc.getScreenCaptureSources(160, 90, 32, 32) ?? []).filter(
      (item) => item.type === SCREEN_CAPTURE_SOURCE_TYPE_SCREEN
    )
  }

  return {
    async listScreenSources(): Promise<RemoteDesktopCaptureSource[]> {
      const trtc = await ensureInstance()
      // 采集源本身不带分辨率；用物理像素兜底。几何要如实回给主控端，
      // 报错了对方的坐标换算会整体偏移。
      const width = Math.round(window.screen.width * window.devicePixelRatio)
      const height = Math.round(window.screen.height * window.devicePixelRatio)
      const sources = listRawSources(trtc).map((item) => ({
        sourceId: item.sourceId,
        sourceName: item.sourceName,
        isMainScreen: item.isMainScreen === true,
        width,
        height
      }))
      // 一个源都没有会一路走到「没有可用的屏幕采集源」，但那时已经进过房了；
      // 在源头记一笔，好区分是采集权限问题还是房间问题。
      log('listed screen sources', {
        count: sources.length,
        names: sources.map((item) => item.sourceName).slice(0, 4),
        width,
        height
      })
      return sources
    },

    async startSharing(
      params: RemoteDesktopEnterRoomParams,
      source: RemoteDesktopCaptureSource
    ): Promise<void> {
      const trtc = await ensureInstance()

      await new Promise<void>((resolve, reject) => {
        // 这 15 秒里 SDK 说过什么，全记下来。超时不带上它就只是一句
        // 「进房超时」，查不下去。
        const seen: string[] = []
        const observers = OBSERVED_ENTER_ROOM_EVENTS.map((event) => {
          const handler = (...args: unknown[]): void => {
            seen.push(event)
            log(`trtc event ${event}`, { args: args.map((a) => summarize(a)) })
          }
          trtc.on(event, handler)
          return { event, handler }
        })

        const onEnter = (result: unknown) => {
          cleanup()
          // 约定：正数是进房耗时（成功），负数是错误码。
          if (typeof result === 'number' && result < 0) {
            reject(new Error(`进房失败：${result}`))
            return
          }
          log('entered room', { elapsedMs: typeof result === 'number' ? result : null })
          resolve()
        }
        const onError = (code: unknown, message: unknown) => {
          cleanup()
          reject(new Error(`TRTC 错误 ${String(code)}：${String(message ?? '')}`))
        }
        const timer = setTimeout(() => {
          cleanup()
          reject(
            new Error(
              seen.length
                ? `进房超时（${ENTER_ROOM_TIMEOUT_MS / 1000}s 内只收到：${[...new Set(seen)].join('、')}）`
                : `进房超时（${ENTER_ROOM_TIMEOUT_MS / 1000}s 内没有收到任何 TRTC 事件）`
            )
          )
        }, ENTER_ROOM_TIMEOUT_MS)
        function cleanup(): void {
          clearTimeout(timer)
          trtc.off?.('onEnterRoom', onEnter)
          trtc.off?.('onError', onError)
          for (const { event, handler } of observers) trtc.off?.(event, handler)
        }

        trtc.on('onEnterRoom', onEnter)
        trtc.on('onError', onError)
        // userSig 是凭证，只记长度和指纹位；房间号和 userId 必须记全，
        // 双机对不上房间时就靠这两行对账。
        log('entering room', {
          sdkAppId: params.sdkAppId,
          userId: params.userId,
          roomId: params.roomId,
          roomIdField: 'strRoomId',
          userSigLength: params.userSig?.length ?? 0,
          scene: TRTC_APP_SCENE_VIDEO_CALL
        })
        // 必须是 TRTCParams 实例，不能是对象字面量：见 resolveTrtcParams 的注释。
        const enterParams = new trtcParamsClass!()
        enterParams.sdkAppId = params.sdkAppId
        enterParams.userId = params.userId
        enterParams.userSig = params.userSig
        enterParams.strRoomId = params.roomId
        trtc.enterRoom(enterParams, TRTC_APP_SCENE_VIDEO_CALL)
      })

      // 必须先选目标再开采集：采集器没有源时一帧都不产出，而且不报错，
      // 对端表现为永远黑屏。MaiChat 当初就栽在漏了这一步。
      const target =
        listRawSources(trtc).find((item) => item.sourceId === source.sourceId) ??
        listRawSources(trtc)[0]
      if (!target) {
        trtc.exitRoom()
        throw new Error('没有可用的屏幕采集源')
      }
      log('selecting capture target', {
        requested: source.sourceId,
        used: target.sourceId,
        name: target.sourceName,
        fellBack: target.sourceId !== source.sourceId
      })
      trtc.selectScreenCaptureTarget(
        target,
        { left: 0, top: 0, right: 0, bottom: 0 },
        { enableCaptureMouse: true, enableHighlightBorder: false }
      )
      // view 传 null：被控端自己不需要预览，省一块渲染开销。
      trtc.startScreenCapture(null, TRTC_VIDEO_STREAM_TYPE_SUB, null)
      sharing = true
      log('screen capture started', { streamType: TRTC_VIDEO_STREAM_TYPE_SUB })
    },

    async stopSharing(): Promise<void> {
      // 停止路径会被多个事件触发（对端 stop、本地停止、IM 断开），必须可重复调用。
      if (!instance) return
      log('stopping', { wasSharing: sharing })
      if (sharing) {
        try {
          instance.stopScreenCapture()
        } catch {
          // 采集本来就没起来：这条路径的目的是回到干净状态。
        }
        sharing = false
      }
      try {
        instance.exitRoom()
      } catch {
        // 已经不在房间里也算达成目的。
      }
    }
  }
}
