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

export function createTrtcRemoteDesktopEngine(): RemoteDesktopEngine {
  let instance: TrtcInstance | null = null
  let sharing = false

  async function ensureInstance(): Promise<TrtcInstance> {
    if (instance) return instance
    // 动态 import 而不是顶层 import：SDK 带 29MB 原生二进制，没开远程桌面的
    // 用户不该在每次启动时都为它付加载成本。
    const mod: unknown = await import('trtc-electron-sdk')
    const holder = mod as { default?: { getTRTCShareInstance(): TrtcInstance } } & {
      getTRTCShareInstance?(): TrtcInstance
    }
    const sdk = holder.default ?? (holder as { getTRTCShareInstance(): TrtcInstance })
    instance = sdk.getTRTCShareInstance()
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
      return listRawSources(trtc).map((item) => ({
        sourceId: item.sourceId,
        sourceName: item.sourceName,
        isMainScreen: item.isMainScreen === true,
        width,
        height
      }))
    },

    async startSharing(
      params: RemoteDesktopEnterRoomParams,
      source: RemoteDesktopCaptureSource
    ): Promise<void> {
      const trtc = await ensureInstance()

      await new Promise<void>((resolve, reject) => {
        const onEnter = (result: unknown) => {
          cleanup()
          // 约定：正数是进房耗时（成功），负数是错误码。
          if (typeof result === 'number' && result < 0) {
            reject(new Error(`进房失败：${result}`))
            return
          }
          resolve()
        }
        const onError = (code: unknown, message: unknown) => {
          cleanup()
          reject(new Error(`TRTC 错误 ${String(code)}：${String(message ?? '')}`))
        }
        const timer = setTimeout(() => {
          cleanup()
          reject(new Error('进房超时'))
        }, ENTER_ROOM_TIMEOUT_MS)
        function cleanup(): void {
          clearTimeout(timer)
          trtc.off?.('onEnterRoom', onEnter)
          trtc.off?.('onError', onError)
        }

        trtc.on('onEnterRoom', onEnter)
        trtc.on('onError', onError)
        trtc.enterRoom(
          {
            sdkAppId: params.sdkAppId,
            userId: params.userId,
            userSig: params.userSig,
            strRoomId: params.roomId
          },
          TRTC_APP_SCENE_VIDEO_CALL
        )
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
      trtc.selectScreenCaptureTarget(
        target,
        { left: 0, top: 0, right: 0, bottom: 0 },
        { enableCaptureMouse: true, enableHighlightBorder: false }
      )
      // view 传 null：被控端自己不需要预览，省一块渲染开销。
      trtc.startScreenCapture(null, TRTC_VIDEO_STREAM_TYPE_SUB, null)
      sharing = true
    },

    async stopSharing(): Promise<void> {
      // 停止路径会被多个事件触发（对端 stop、本地停止、IM 断开），必须可重复调用。
      if (!instance) return
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
