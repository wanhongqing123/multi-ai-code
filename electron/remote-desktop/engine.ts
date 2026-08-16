// TRTC 引擎的抽象接口。
//
// 真实现依赖 trtc-electron-sdk，而那个包只能在渲染进程加载（主进程 require 会
// 报 document is not defined：它的 index.js re-export 了 Renderer / MediaMixingDesigner
// 等模块级就访问 DOM 的模块）。把引擎放在接口后面有两个好处：
//   1. controller 的判定逻辑可以用 fake 完整单测，不需要真进房、不需要网络
//   2. 真实现换到渲染进程也不影响这里的调用方
//
// 输入注入的热路径全在引擎那侧（preload）：TRTC 的自定义消息在那里到达，
// koffi/SendInput 也只能在那里调。渲染层不逐事件参与，只在门禁状态变化时
// 下推一次 setInputGate——30Hz 的鼠标移动如果每条都跨进程往返，延迟和
// 开销都不可接受。判定本身仍是 session.ts 里那个纯函数，两侧不会各判一套。

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
  /**
   * 下推远程输入门禁状态。
   *
   * gate 为 null 表示不接受任何远程输入（未共享、或用户没开控制开关），
   * 引擎收到后会把按住的键全抬起——切断控制时不能留下 Ctrl 卡住的状态。
   */
  setInputGate(gate: RemoteDesktopInputGate | null): void
}

/** 允许注入的前提条件。任一项对不上，引擎就丢弃收到的输入包。 */
export interface RemoteDesktopInputGate {
  sessionId: string
  /** 只认当前会话的对端：房间里若混进第三方，它的输入一律不执行。 */
  peerUserId: string
  /** 被采集屏幕在虚拟桌面里的位置，归一化坐标按它换算回像素。 */
  captureScreen: { left: number; top: number; width: number; height: number }
}
