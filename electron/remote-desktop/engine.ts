// TRTC 引擎的抽象接口。
//
// 真实现依赖 trtc-electron-sdk，而那个包只能在渲染进程加载（主进程 require 会
// 报 document is not defined：它的 index.js re-export 了 Renderer / MediaMixingDesigner
// 等模块级就访问 DOM 的模块）。把引擎放在接口后面有两个好处：
//   1. controller 的判定逻辑可以用 fake 完整单测，不需要真进房、不需要网络
//   2. 真实现换到渲染进程也不影响这里的调用方
//
// 本轮只做「看画面」：接口只暴露进房 + 屏幕采集，不含任何输入注入。

/** 与 MaiChat 对齐的采集几何，accept 信令里要原样回给主控端。 */
export interface RemoteDesktopCaptureSource {
  sourceId: string
  sourceName: string
  isMainScreen: boolean
  width: number
  height: number
}

export interface RemoteDesktopEnterRoomParams {
  sdkAppId: number
  userId: string
  userSig: string
  /** 字符串房间号，由主控端在 invite 里给出。 */
  roomId: string
}

export interface RemoteDesktopEngine {
  /** 枚举可采集的屏幕。返回空数组表示这台机器没有可用采集源。 */
  listScreenSources(): Promise<RemoteDesktopCaptureSource[]>
  /**
   * 进房并开始推屏幕共享（辅路）。
   *
   * 必须在 startScreenCapture 之前 selectScreenCaptureTarget——采集器没有源时
   * 一帧都不产出，而且不报错，表现为对端永远黑屏。MaiChat 当初就栽在这里。
   */
  startSharing(
    params: RemoteDesktopEnterRoomParams,
    source: RemoteDesktopCaptureSource
  ): Promise<void>
  /** 停止推流并退房。重复调用必须安全：停止路径会被多个事件触发。 */
  stopSharing(): Promise<void>
}
