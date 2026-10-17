# MaiChat Desktop 视频界面与工具调用密度

- 时间与现场：2026-10-17，macOS 26.4、Apple M3 Pro，安装版 Desktop 2.1.2。用户提供的创作中心截图中，视频弹窗的 Qt 播放条与独立灰色按钮占用画面下方；`agent_send_media` 视频卡片的默认 `QPushButton` 绘成白色方块；同一回答的多次工具调用各占一行。源码基线为 `2ef6f073d`。当前安装版运行中，未停止或替换。
- 对照：iOS `MaiFfplayVideoController` 使用 Graphics 的 `CAMetalLayer` 播放画面，UIKit 控件覆盖在画面上，提供暂停、拖动、倍速、重播、分享、保存、复制路径及相邻媒体浏览。Mac 的视频画面原本已经由 Graphics 渲染；问题位于 Qt 承载控件及交互布局。按用户后续要求，Mac 保留 Qt 承载，未改为独立 AppKit 播放器。
- 修复提交：`f17e0771b1f2f65253533dc127dc8ff8cc8f56d6`。macOS 视频弹窗改用覆盖在 Graphics 画面上的半透明 Qt 控制条，并把原生标题栏设为深色；新增重播、全屏及同一会话视频的上一段/下一段，工具入口也提供 `replay`、`previous_item`、`next_item`。切换媒体复用窗口并保留倍速，转发动作取当前视频路径。Windows 的原有播放控件样式分支保留。创作中心把同一助手消息的工具调用收成一条可展开摘要，授权时自动展开，完成后收起；`agent_send_media` 视频卡片改为深色圆形播放键与 16:9 封面，图片卡片维持原尺寸。
- 验证：Mac 产品目标构建成功，`MaiFfplayHostMacTests`、`MaiFfplayVideoDialogTest`、`MaiGraphicsPresenterMacTests`、`agent_panel_session_test` 4/4 通过。真实 Wan 与 Kling 1080p MP4 在同一播放器窗口完成前后切换、暂停继续、1.5× 倍速保持及转发路径核对；目视检查整窗截图，深色标题栏、画面和浮层均可见。测试会话交付两个视频时，工具摘要显示“工具调用 2 次”，两张卡片都有封面、圆形播放键，点击卡片得到包含两段视频的播放列表。
- 工作目录跟进：`1733a9cce503e58f1d51fc9a4339afae479428f8` 将新会话默认工作区移到应用本机数据目录，并把目录选择移进右上角菜单；该菜单还可打开当前工作目录。旧会话的自选路径保留。菜单截图已目视检查，相关会话测试通过。
- 边界：Mac 可移植 App 位于 `MaiChat/desktop/dist-video-ui-aligned/MaiChat-macos-arm64/MaiChat.app`，打包后签名验证通过。用户正在运行的 `/Applications/MaiChat.app` 未被停止或替换，因此原窗口仍显示旧界面。iOS/Android 代码未改；Windows 构建与实机播放未在这台 Mac 上验证。
