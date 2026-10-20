# Android 会话侧栏展开时未实时更新（2026-10-20）

## 现场与复现

- 设备：OPPO PKD130，Android 15；正式签名 MaiChat 2.2.3 (223)，APK SHA-256
  `c5150bf0275da16ab6d80249aa3226778d0e2847d8ba0491e27218ef726cd565`，
  源码 `83c87c5ba97291d71c6a1ee1a14e67a8d2f5a876`。
- 同一测试账号下，保持创作中心「对话」侧栏展开，让无付费假 Mac 经生产 WSS
  上报新 Session；侧栏未即时出现。收起再打开，目录和「在线」状态正确。
  假 Mac 清空目录并注销后，展开的侧栏仍保留旧行；收起再打开后才消失。
  只读打开远端历史成功，说明目录/历史链路已通；现场观察本身不能单独证明
  WebSocket 推送丢失。

## 根因证据

`AIAssistantController.acceptRemoteCatalog()` 把目录放到 worker 上更新并调用
`publishState()`；`AIAssistantPanel.onState()` 收到新 State，却只更新主消息区。
侧栏 `sessions()` 每次打开时临时创建 `PopupWindow` 与 `sessionList`，仅用当时的
`state.data.sessions` 加一次行，之后没有保存视图引用或刷新入口。因此底层状态
更新后，展开中的 PopupWindow 仍显示打开时的快照；重新打开才读取新 State。

新增 Android 模拟器设备测试
`AIAssistantInstrumentedTest#openSessionDrawerTracksRemoteCatalogPushes`：先展开
空侧栏，再注入远端 Session，要求同一侧栏立刻出现；随后注入空目录，要求同一
侧栏移除旧行，且草稿不变。原代码首次出现断言稳定失败；修复后同一用例通过。

## 修复与验证

产品/测试提交：`62d4ae5299c7ee5e37e5ed89439afa547f8914c4`。
侧栏保持一个 PopupWindow；State 变化时仅在可见行内容变化的情况下原位更新
列表，保留滚动位置。关闭侧栏或离开页面时释放视图引用。远端行点击、打开
失败保留草稿的既有设备测试也通过；Android Debug/Release Java、146 项单测
和行宽检查通过。生产 WSS 服务端没有改动。

## OPPO 修复包验收与边界

从最终文档提交 `2397b385e04ae03fbf594bc057def2c333722363` 重签
MaiChat 2.2.3 (223) APK：159805729 B、SHA-256
`8032b4333c0cb976be95cee695a0b18bc5656502efcd1f4ddf7cf82013f194eb`，
证书 SHA-256 `84187afba7b96fc21b8b4af6c658a56b2b4223749db197cb18c442d5bfe54256`。
OPPO PKD130 保留数据原位升级成功，设备回拉的 `base.apk` 大小与哈希均和候选一致。

真机保持**同一个**侧栏展开且未触碰屏幕，生产 WSS 假 Mac 上报新 Session 后，
「无付费跨端 WebSocket 验收 / 生产验收假 Mac · 在线」原位出现；上报空目录并
下线后，该行原位消失。两方向均有现场截图与 UI XML 核对，没有关开侧栏。
「未发送草稿」行一直存在，收起侧栏后输入框中的 `wss_draft_keep_223` 完整保留。
测试后假 Mac 目录已清空、Mac 登录已注销；手机草稿已清空、测试账号已退出，
修复版 APK 保留安装且设备哈希未变。未触发模型、命令或付费任务。

这证明 Android 2.2.3 修复包的生产 WSS 目录推送和展开态可见刷新；真实
Desktop 产品所属的 Session 执行与多设备同时发送操作仍未由此验收。
