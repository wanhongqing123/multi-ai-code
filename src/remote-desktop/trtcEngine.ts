// 渲染进程侧的引擎代理。
//
// 真实现在 preload（electron/remote-desktop/preloadEngine.ts）：应用是
// nodeIntegration:false + contextIsolation:true，渲染进程既拿不到 require，
// 打包器也会试图把 SDK 的 .node 二进制打进 bundle。preload 两样都没有这个问题。
//
// 这里只是把接口调用转发过去，不含任何判定。

import type { RemoteDesktopEngine } from '../../electron/remote-desktop/engine.js'

export function createTrtcRemoteDesktopEngine(): RemoteDesktopEngine {
  const bridge = window.api.remoteDesktop
  return {
    listScreenSources: () => bridge.listScreenSources(),
    startSharing: (params, source) => bridge.startSharing(params, source),
    stopSharing: () => bridge.stopSharing()
  }
}
