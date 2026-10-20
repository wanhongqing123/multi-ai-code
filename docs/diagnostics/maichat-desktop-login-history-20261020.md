# Desktop 登录页、设置页与跨端会话回归（2026-10-20）

## 现场和复现

- 平台：macOS 26.4，用户运行 `/Applications/MaiChat.app` 2.2.6；该已发布版本对应源码 `83c87c5ba97291d71c6a1ee1a14e67a8d2f5a876`。本轮构建树不会自动替换该进程。
- 15:22 登录页同时展示扫码和账号密码表单；Mac 弹出对 `com.maichat.desktop.auth` 的登录钥匙串访问提示。设置页截图在 15:24 显示连接状态、SDK AppID、登录签名、创作中心模型四行内部信息。
- 15:26 消息区与输入框之间出现浅蓝灰横线；用户 15:32–15:34 的截图显示，打开 iPhone 所属 Session 时先“正在读取”，随后“操作状态未知”，偶尔才出现历史；远端输入框是独立的灰色发送按钮，没有复用本地创作中心输入样式。
- 这些截图只证明当时的界面，不证明用户点击了发送。排障没有从用户设备提交模型任务。

## 根因证据

- 登录 UI 在 `LoginDialog::buildUi` 把扫码和密码控件依次加入同一个纵向布局，导致两个完整表单同时出现。`SecureRefreshTokenStore` 对该 service 用 macOS `SecItemCopyMatching` 读取账号刷新令牌；当前 Mac 2.2.6 的包是 ad-hoc 签名，更新 App 后系统可要求重新授权旧钥匙串项。提示中的密码属于 Mac 登录钥匙串，不是 MaiChat 账号密码。
- `videoContentSplitter_` 的样式使用宽泛的 `QSplitter::handle`，沿 Qt 样式树影响内部的 `messageComposerSplitter`。原生 Mac 截屏中的横线像素为 `#dbe4ef`，恰好是视频分栏柄的样式颜色；新增 `composerResizeHandleBlendsIntoCanvas` 在修前失败并报出该颜色。
- `RemoteAgentPanel` 每 5 秒对已选 Session 重发最近 10 条历史请求，即使上一页仍在途。生产服务端只读元数据统计：10 分钟内单账号有 941 个 iPhone→Mac `history_page` 分块，JSON 载荷总长约 29.6 MB，大量分块尚未确认；这是重复读取和排队的证据，不代表传输了媒体文件实体。iOS 和 Android 的历史序列化还包含界面不用的工具输入输出、推理文字等字段；媒体路径虽已剥离，文字负载仍大。
- Desktop 的 15 秒 WebSocket 请求超时对所有 `send` 都显示“操作状态未知”，把可重试的历史读取误写成带副作用操作的不确定状态。原远端输入框是普通 `QTextEdit` 加外置 `QPushButton`。
- 完整 CTest 首轮还触发已有测试夹具的悬空引用：`coordinatorReadsRemoteHistoryWithVerifiedChunks` 结束时留下一条 `QTimer::singleShot` 回调，稍后写入已销毁的局部 `QString`。LLDB 栈停在该回调的 `QString::operator=`；测试等待回调后不再复现。

## 修复

产品与回归提交：`f0898f7e`。

- 登录页右上角切换“账号密码登录”／“扫码登录”，同一时刻只显示一个表单。离开扫码页立即停止倒计时与轮询、取消旧请求，并继续撤销可能晚到的授权；密码表单保留账号草稿，错误显示在当前表单。
- 设置页删除四行内部信息，保留本人资料、昵称、当前账号、退出登录和远程桌面设置；相应文案同时支持扫码和账号密码重新登录。
- 视频分栏柄的 QSS 只匹配 `#videoContentSplitter`，消息输入区的可拖拽柄保持透明；拖动调整输入区高度的行为保留。
- 远端面板进入时取最近一页、查看更早时按需取页；可见时最多每 30 秒取最新一条，未完成的读取不会重叠。读取超时使用读取错误文案，失败仍保留已有历史和草稿。iOS、Android 与 Desktop 所属端只回传可见文字摘要、工具名称／状态和媒体占位所需字段，剔除工具输入输出、推理正文和媒体路径；不改变本机完整会话数据。
- 远端输入区复用 `ComposerTextEdit` 的外壳与内嵌发送动作；Enter 发送、Shift+Enter 换行。跨端附件仍未支持，左下角加号不可用且说明原因。

## 验证与边界

- Mac 原生 Qt：横线像素回归修前 `#dbe4ef` 失败、修后通过；登录／设置／远端面板的截图已人工查看。`desktop_session_sync_host_test`、`login_dialog_test`、`main_window_layout_test` 完整 CTest 3/3 通过，分别覆盖 9 项 Host 用例和相关 UI 旧路径。
- iOS ARM 模拟器正常 App 编译通过；Android JDK 17 Debug Java 编译与 `testDebugUnitTest` 通过。`npm run check:line-width` 与 `git diff --check` 通过。
- 当前用户 Mac 仍运行已发布 2.2.6，未被本轮构建替换；iPhone 在尝试后续真机验证时显示 unavailable。新代码尚未在用户安装版、Windows 原生版本或真实 iPhone→Mac 新版组合上验收，也未发布新 Release。生产队列中的旧临时历史分块按服务端 10 分钟 TTL 自行过期，本轮未删除用户数据。
