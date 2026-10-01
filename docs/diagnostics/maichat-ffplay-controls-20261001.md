# FFplay 控制命令未实际执行（2026-10-01）

## 现场与复现

- Windows 同伴在 D3D11 真机播放中发送 `seek_percent`，原版 `ffplay.c`
  打印 `Seek to inf%`。旧共享测试只断言命令进入事件队列，没有验证播放状态。
- 源码基线 `e141f035`，运行的是该基线上的 Windows hosted FFplay DLL；
  对应 Mac 的 `MaiFfplayHostMacTests` 也采用了相同的队列断言。

## 根因证据

- `ffplay.c` 的键盘处理在 `VideoState.width == 0` 时直接跳过；鼠标百分比
  seek 则用这个宽度作除数。它会在 `SDL_WINDOWEVENT_SIZE_CHANGED` 中更新
  `VideoState.width/height`，首次视频呈现的 `video_open` 也会设置尺寸；
  但在首次呈现之前，或尚未完成视频初始化时，不能靠后者保证控制可用。
- `MaiChat/Media/FfplayCompat/MaiFfplayRenderer.cpp` 原先只更新适配器自己的
  `SDL_Window.width` 和 `sWindowWidth`，没有把尺寸变化事件送给 FFplay。
  因此适配器知道宽度，并不等于 FFplay 的 `VideoState` 已有宽度。

## 修复与验证

- 共享尺寸事件及实际控制回归提交：`4ce5f972745baddcca818d17f4645e7123a1a8a2`。
- 适配器在创建窗口与 `SDL_SetWindowSize` 时入队 `SDL_WINDOWEVENT_SIZE_CHANGED`，
  保持复制的 `ffplay.c` 字节不变。销毁窗口时清除适配器宽度。
- 测试现在等待实际呈现帧，确认暂停后画面停止、恢复后继续出帧，并通过
  FFplay 本身的日志确认百分比 seek 进入处理路径且百分比不是 `inf/nan`；
  同一进程连续播放两次。Mac 的 H.264 与 AV1 素材都通过。
- iOS/Android 构建与现有播放器 UI 测试结果在本次提交前核对；Windows
  D3D11 真机复测由 Windows 同伴基于修复提交完成。

## 边界

- 队列接受命令仍不能作为执行证明；以后新增播放命令应断言可观察的
  播放状态或 FFplay 实际处理日志。
- 此修复解决宽度零和百分比 seek 无穷值，不代表 ffplay 所有选项已在
  MaiChat UI 与 Agent 工具中暴露或逐平台验收。
