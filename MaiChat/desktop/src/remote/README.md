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

## 二期「远程操控」的扩展点

一期只做看屏幕。加鼠标键盘操控时按下面的位置插入，**不需要重构现有分层**：

1. **信令**：`RemoteDesktopSignal::Type` 增加 `RequestControl` / `GrantControl` /
   `RevokeControl`。枚举 + 字符串映射两处，协议版本字段已就位。

2. **权限状态**：操控权是**独立于会话状态的正交维度**，应在
   `RemoteDesktopSession` 里加 `bool controlGranted`，而不是往 `HostState`
   里塞 `SharingWithControl` —— 后者会让状态数量翻倍。

3. **输入事件通道**：`ITrtcEngine` 增加
   `sendCustomMessage(QByteArray)` 与 `customMessageReceived` 回调
   （底层是 TRTC `sendCustomCmdMsg`。配额 **30 条/秒、16KB/秒，是整个客户端
   的总量**：源码 `trtc_message_sender.cc` 里 `sendCustomCmdMsg` 与 `sendSEIMsg`
   走同一个 `CheckIfCanSendMessage`、共用同一个计数器。实际阈值 40，文档写 30，
   我们按 30 走留安全垫。被拒的消息照样计数，所以发送端用滑动窗口匀速发 +
   按键优先，永不触发突发惩罚）。
   接口加方法后，Null 实现和 fake 各补一个空实现即可。

4. **新增两个纯模块**（与现有同级，仍可完整单测）：
   - `RemoteInputEvent`：鼠标/键盘事件的编解码 + 坐标归一化
     （远端分辨率 ↔ 本地视图尺寸的换算是纯函数，必须单测）
   - `InputInjector`：`IInputInjector` 接口 + Win32 `SendInput` 实现 + fake。
     注入是不可逆副作用，**必须**放在接口后面，否则测试会真的动鼠标。

5. **安全**：操控权需要二次授权（不能因为对方能看就自动能控），
   走与 invite 同样的"状态机决策 + controller 执行"路径。

## 测试

每个模块配一个测试目标，见 `tests/RemoteDesktop*Test.cpp` 与 `TrtcEngineTest.cpp`。
controller 用 fake engine 断言"是否真的进房推流"，不联网。
