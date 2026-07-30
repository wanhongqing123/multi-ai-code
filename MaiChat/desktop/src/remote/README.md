# remote/ — 远程桌面模块

MaiChat ↔ MaiChat 的远程桌面。画面走腾讯 TRTC，信令借道现有 IM 文本通道，
Electron 端不参与。

## 分层

自下而上，上层依赖下层，**下层不反向依赖上层**：

```
┌─ UI（ui/ 目录）──────────────────────────────────────┐
│  设置页分区 · 同意弹窗 · 共享指示条 · 观看窗           │
└───────────────▲──────────────────────────────────────┘
                │ 信号/槽
┌───────────────┴──────────────────────────────────────┐
│  RemoteDesktopController   粘合层（唯一有副作用的地方）│
│  只做三件事：翻译信令 / 委派决策 / 执行副作用          │
└──▲───────────▲──────────────▲────────────▲───────────┘
   │           │              │            │
┌──┴────┐ ┌────┴──────┐ ┌─────┴─────┐ ┌────┴────────┐
│Signal │ │  Session  │ │   Auth    │ │ TrtcEngine  │
│编解码 │ │  状态机   │ │ 密码校验  │ │  SDK 封装   │
└───────┘ └───────────┘ └───────────┘ └─────────────┘
              ↑ 纯函数，无 Qt UI / 无网络 / 可完整单测
          ┌───┴────────────┐
          │ Settings 持久化 │
          └────────────────┘
```

**核心约束：所有安全判断只在 `RemoteDesktopSession` 里做。**
controller 不自己决定"要不要放行"，只把状态机的决策翻译成动作。这样
"什么情况下会自动共享屏幕"这类问题永远只需读一个文件，也能被完整单测。

## 各文件职责

| 文件 | 职责 | 依赖 |
|---|---|---|
| `RemoteDesktopSignal` | invite/accept/reject/stop 的编解码 | 无 |
| `RemoteDesktopAuth` | PBKDF2 密码哈希、HMAC proof 生成与校验 | 无 |
| `RemoteDesktopSession` | 双端状态机与全部安全决策 | Signal, Auth |
| `RemoteDesktopSettings` | 模式/密码凭据/白名单/失败计数落盘 | Auth, Session |
| `TrtcEngine` | `ITrtcEngine` 接口 + TRTC 实现 + 空实现 | 无（SDK 在 vendor） |
| `RemoteDesktopController` | 粘合与副作用执行 | 以上全部 |
| `RemoteInputProtocol` | 输入事件编解码、可靠/不可靠通道与坐标归一化 | 无 |
| `RemoteKeyMapping` | Qt 按键 ↔ 协议规范键码 ↔ macOS CGKeyCode | 无 |
| `RemoteInputSender` | 限流、合包与移动事件降频 | Protocol |
| `RemoteInputInjector` / `RemoteInputSink` | 会话校验、悬空键兜底与 Win/macOS 系统注入 | Protocol, KeyMapping |

## 远程操控

Windows 与 macOS 均可作为控制端和被控端，支持四种组合：

| 控制端 | 被控端 | 输入注入 |
|---|---|---|
| Windows | Windows | Win32 `SendInput` |
| Windows | macOS | `CGEventPost` |
| macOS | Windows | Win32 `SendInput` |
| macOS | macOS | `CGEventPost` |

输入协议使用 Windows VK 值作为平台无关的规范物理键码。控制端从 Qt 键值
转换为规范码，被控端再转换为本机键码，不能直接发送
`QKeyEvent::nativeVirtualKey()`：macOS 的 A 键原生值就是 0，而且与 Windows VK
完全不是同一套编号。

鼠标移动走 TRTC 不可靠通道，按键、点击、滚轮、文本和 `ReleaseAll` 走可靠
通道。发送端遵守 `sendCustomCmdMsg` 的频率和带宽限制；接收端按会话 ID、
序号和静默超时释放悬空按键。

macOS 被控端必须获得系统“辅助功能”权限，屏幕共享还需要“屏幕录制”权限。
设置页会显示授权状态。共享期间 Windows 和 macOS 都会阻止自动休眠，结束
会话后恢复系统默认策略。

## 测试

每个模块配一个测试目标，见 `tests/RemoteDesktop*Test.cpp` 与 `TrtcEngineTest.cpp`。
controller 用 fake engine 断言"是否真的进房推流"，不联网。
