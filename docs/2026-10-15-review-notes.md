# 2026-10-15 代码审阅索引

这份索引说明本次可审阅的实现和验证范围。文件注释已单独提交，功能与测试改动另行提交，便于分开核对。

## iOS 白屏与消息列表

- `MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantModel.swift`：创作中心先读取目标会话最近一页，再一次性切换可见消息；首屏最多 8 条，向上阅读时每次开放 20 条。
- `MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift`：当前页使用实际消息行排版；切换会话、新消息和播放器改变视口时定位稳定消息 ID。
- `MaiChat/ios/MaiChat/MaiChat/IM/RemoteIMAppState.swift`：普通 IM 首屏最多 20 条；搜索或引用旧消息只显示命中项及其前面最多 9 条，避免把后续数千条历史交给视图。
- `MaiChat/ios/MaiChat/MaiChat/Chat/ChatView.swift`：上滑到当前页顶部按需读取旧页，并保持原消息锚点；旧消息窗口右下角提供回到最新的按钮。
- `MaiChat/ios/MaiChat/MaiChat/Chat/MessageBubbleLayout.swift`：只在实际内容或视口尺寸变化后修正遗留越界偏移；用户拖动和系统回弹期间不强制夹取。
- `MaiChat/ios/MaiChat/MaiChat/App/RootView.swift`：创作中心首次打开后保留同一页面实例，视频停靠只改变可用视口。

根因证据：真机白屏期间，日志仍记录到已加载的消息和正常的消息条数；旧 IM 页面曾把 2544 条消息交给懒排版列表，创作中心的长会话也把已加载消息整批交给 `LazyVStack`。滚动偏移与实际已排版行不一致时，视口可能落到空白处。语音顶层悬浮层和播放器分屏改变可用高度，增加了触发机会，但没有证据表明识别结果会清空聊天数据库。

相关历史：IM 懒消息容器来自 `f2723e64f9`；创作中心懒消息容器来自 `0093593391`；顶层语音悬浮层来自 `d9332aae`。这是多个条件叠加的回归，不能仅凭一条提交就断定全部原因。

## 语音转文字悬浮层

`ChatView.swift` 中的转写卡片固定在屏幕中间，文字按实际排版高度扩展；取消和编辑目标单独定位在长按点上方 160pt。`MaiChatApp.swift` 把悬浮层放在窗口顶层，使播放器停靠时仍可使用。识别结束后先等待 SDK 返回最终文本，展示约 240ms 再收起；入场使用短促弹出动画。悬浮层不参与聊天消息区的布局测量。

## 中文注释与命名

按用户澄清的口径，注释比例是约数，不要求逐行配对。已核对 672 个被 Git 跟踪的自有源码与脚本：每个文件开头都有中文文件作用说明；其中补齐了此前漏掉的 MaiChat 媒体层和桌面测试文件。上游 `MaiChat/Media/ffplay.c`、第三方 Framework、`vendor`、`third_party` 和 `MaiAgent/Graphics` 保持原样。长函数按入口、关键阶段和失败边界补注释；静态扫描找出 406 个超过 50 行的候选代码块，排除类、lambda 等误判后，没有发现缺少注释的候选函数。该扫描是审阅辅助，不代替逐个核对语义。`MaiSqliteStore.cpp` 内部的 `exec`、`runWith`、`step` 已改成表明事务控制、文本参数和预处理语句职责的名称，`MaiStoreTests` 通过；全仓库命名审阅仍需继续。

## clangd

clangd 的标准文件名是 `compile_commands.json`。可重建入口为：

- `MaiAgent/scripts/generate-compile-commands.sh`
- `MaiChat/desktop/scripts/generate-compile-commands.sh`
- `MaiChat/android/MaiChat/scripts/generate-compile-commands.sh`
- `MaiChat/ios/MaiChat/scripts/generate-compile-commands.py`

本机当前数据库分别覆盖 MaiAgent 452 条、桌面端 1226 条、Android 原生 1433 条、移动 Agent 适配层 1109 条、iOS 工程 2 条编译命令。Android JNI 中两处旧标准库可能返回 `const char*` 的 `std::string::data()` 写入已改为非空字符串首元素访问，并通过 arm64 原生构建。

## 已运行的检查与剩余限制

- iOS 模拟器：正式 App 使用本机测试账号进入创作中心并切换已有会话；本轮改为显式测试开关后重新运行了该 UI 用例并人工检查截图，切换后消息仍可见。另以 80 条创作中心历史快速上下滑动、2544 条合成 IM 历史检查首屏、旧页加载、旧搜索命中与返回最新。未调用付费模型。本机假账号服务的 IM AppID 只是占位值，所以这组用例不能证明两台客户端真实互发 IM。
- 测试账号入口：iOS 仅在 Debug 模拟器收到 `MAICHAT_LOCAL_UI_PHONE` 时使用本机假账号服务；Android 仅在 Debug 模拟器以 `-PmaichatLocalUiAuth=true` 构建时使用。普通调试、真机与发布包继续连接正式账号服务。本机服务脚本关闭付费模型签发。
- iOS 设备：已安装包含搜索命中补漏的构建，但安装后没有自动打开或替用户做真机交互，因此不能宣称最后一版在真机上已完成体验验证。
- Android：OpenJDK 17 的 Java 构建通过，arm64 JNI 原生构建通过。GraalVM 和 Android Studio JBR 在该工程的 `jlink` 转换阶段失败，属于构建环境差异。
- 桌面端：`maichat` 主目标完整构建通过。
- MaiAgent：完整构建通过，43 项测试全部通过。`MaiSpecialistNotificationTests` 的旧断言曾要求把状态查询异常和暂缺产物终结为生成失败；现已把用例改为核查任务继续运行且不产生失败通知，生产轮询代码未变。

本轮补注释后，桌面端 `maichat` 目标与 Android Debug APK 均重新构建通过；iOS 涉及的 Swift 文件已通过解析检查。此轮只改注释、原有不准确的缩进和上述测试断言，没有重新安装真机版，因此真机上的最终白屏体验仍需按已知路径复核。

当前工作区还存在事先已有的 ONNX Runtime 上游测试目录等未跟踪内容；它们不属于本次注释或白屏修改，也没有被纳入审阅范围。
