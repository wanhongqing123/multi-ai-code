# MaiChat Agent 重复批准弹窗（2026-10-22）

## 现场与复现

- 平台：iPhone 上的创作中心；Desktop 的同名媒体工具具有相同规则。
- 操作：向 Agent 说“打开相机拍照”或“播放音乐”；在相机预览／音乐来源选择界面出现之前，Agent 先弹出批准框，每次调用都会重复。
- 预期：直接打开可见的相机预览或音乐选择／播放界面；系统首次权限和用户按快门、选歌、拨号确认、短信发送仍由各系统界面控制。

## 根因证据

- `MaiChat/MaiChatTools/MaiMobileHostTools.cpp` 的 `MaiMobileHostTool` 把一个 `approval` 布尔值同时用于 `requiresApproval` 与 `requiresPerCallApproval`；拍照、音乐此前注册为 `true`。
- `MaiChat/desktop/src/MaiChatTools/DesktopMediaTools.cpp` 的拍照和音乐工具此前也把两个审批函数都设为 `true`。
- `MaiAgent/Agent/MaiTurnRunner.cpp` 的 `checkPermission` 在 `requiresPerCallApproval` 为真时跳过会话级已批准缓存，因此每次必弹；这不是系统相机或音乐库权限弹窗。

## 修复与保留边界

- 修复提交：`3635be22`。拍照和音乐工具取消 Agent 批准；iOS `tel:` 仍由系统要求用户确认拨号，短信仍由系统编辑器让用户检查并点发送，因此这两个“仅打开系统确认界面”的工具也去掉额外 Agent 批准。
- 直接向他人发送 MaiChat 文本／媒体／回复／群发、好友请求及上传或付费委托仍保留逐次批准，因为工具调用会直接产生外部影响。
- 文件修改、Shell、网络、照片导出与系统数据读取仍分别受 Agent 权限策略、会话批准或操作系统权限约束；本次没有放宽这些工具。

## 验证与未验边界

- 移动 Agent 本地集成测试：模拟拍照和音乐调用后均没有待批准请求，宿主工具各执行一次；完整测试通过。
- Mac Desktop：拍照和音乐工具的两个审批函数均为 false，FFplay 视频弹窗定向测试通过。
- 系统权限仍按原有相机、音乐库实现请求；真实 iPhone 尚待安装新构建后检查 Agent 相机／音乐弹窗。Windows 运行未验证。
