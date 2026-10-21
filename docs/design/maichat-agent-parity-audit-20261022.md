# MaiChat iOS 与 Desktop Agent 能力核对（2026-10-22）

## 范围与版本

- 对照源码：`a8474c0cee0d649f48554580d099d8d93b327e1a`。Mac/Windows Desktop 2.2.12 已发布；本机 `/Applications/MaiChat.app` 的 `CFBundleShortVersionString` 与实际运行进程仍为 2.2.11，不能以源码测试代表现装 App 已更新。
- 本次是工具注册、调用链和既有定向测试的核对，没有触发付费模型、真实跨设备命令、电话或短信。供应商额度、凭据和系统权限仍会影响实际可用性。

## 已共用或语义接近

| 能力 | iOS | Desktop | 证据 |
| --- | --- | --- | --- |
| Agent 核心、SQLite 会话、审批 | `MaiMobileAgent` 包装同一 `MaiAgent` | `AgentController` 直接构造 `MaiAgent` | `MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp`、`MaiChat/desktop/src/agent/AgentController.cpp` |
| 主聊天模型 | `glm-5.3`、`glm-5.3-flash`、`deepseek-flash` | 同三项 | `AIAssistantModel.swift`、`AgentChatPanel.cpp` |
| 模型专业工具 | GLM、MiniMax、Wan、百炼可灵的图像／视频工具；Seedance/Seedream 暂不接新任务 | 同样接线和限制 | 两端 Agent 组装函数与系统提示词 |
| 本地处理 | 文件、网络、SSH、FFmpeg、FFprobe、OpenCV、PDF | 同类能力；桌面还可执行 shell | 两端工具注册函数 |
| MaiChat IM 工具 | 联系人、好友申请、昵称、会话／消息读取与检索、未读、文本／媒体／引用回复／群发 | 同 11 个工具名 | `MaiChat/MaiChatTools/MaiMobileHostTools.cpp` 与 `MaiChat/desktop/src/MaiChatTools/MaiChatHostTools.cpp` |
| 视频播放 | 同名 `maichat_play_video` 和 `maichat_video_command` | 同名工具、同一 FFplay 基础 | iOS 宿主工具与 `DesktopMediaTools.cpp` |
| 设备会话同步 | 带 `reauth` 的 WSS 及远端文字历史／指令 | 带 `reauth` 的 WSS 及远端阅读区／指令 | `AIAssistantModel.swift`、`DesktopSessionSyncChannel.cpp` |

## 不能称为完全对齐的点

1. **同名媒体发送工具的说明文字语义不同。** Desktop `maichat_send_media` 将 `caption` 交给媒体消息发送函数；iOS 先发送媒体，再把非空说明作为第二条文本消息发送，返回 `caption_sent`。若产品要求跨端看到同一条媒体带说明，需要统一协议与失败回滚语义。
2. **同名视频打开工具的起播参数不同。** iOS `maichat_play_video` 接受 `start_seconds`；Desktop 的 Schema 只有 `path`，桌面用户不能在打开时用同一参数直接从指定秒数起播。两端后续 `maichat_video_command` 也有平台独有动作。
3. **专业视频工具仍有平台差异。** iOS 装配本地去水印专用工具，Desktop 系统提示词明确声明该工具只在 iOS；不能宣传为三端都有。
4. **本机系统能力不同。** iOS 有相册、系统联系人、拨号、短信编辑器、前台定位与手机权限工具；Desktop 有 shell、窗口截图、子 Agent 和可访问本机任意工作目录的工具。这些是有意的平台边界。
5. **跨设备附件尚未对齐。** Desktop 远端面板与 iOS 远端会话均有“不支持跨设备附件”的路径；两端已对齐的主要是文字历史和指令，不代表远端视频文件可直接传输与播放。

## 结论

共用 Agent 主干已接同一核心和同类模型／媒体工具，不能宣称功能、入参和消息表现完全一致。优先统一同名 IM 媒体工具的 `caption` 语义，再统一视频工具的 `start_seconds`；随后用两台真实设备和隔离账号验证一项媒体生成、工具审批、跨端浏览及撤回全过程。当前已发布的 2.2.12 主要改变菜单外观，不包含上述协议差异修复。
