# Desktop 原生密码表单的前台验收条件

## 实现范围

- 日期：2026-10-29（Asia/Shanghai）。本轮工作树新增 Desktop 的 MaiChat 登录密码普通 API、Settings/Agent 共用安全表单，以及 PDF 打开密码的私有宿主表单；产品提交和最终产物由统一构建执行者补充。
- 登录密码只有本地 `QLineEdit::Password` 与普通 `auth/password/set` 请求接触明文。请求状态表只存 ID、原账号登录身份和状态；工具 `maichat_change_password` 只接 `open/status/request_id`，不接密码。HTTP 200 本身不算成功，须有布尔 `password_set=true`。登录 generation 贯穿令牌刷新重试和回复检查，同账号重新登录也不能沿用旧表单。
- 原生表单实际 show 后才标记 opened/awaiting_user；请求在途为 submitting，禁止关闭误报 canceled。修改后其他设备会退出登录、当前设备保留登录的说明保留。没有给完全访问模式额外增加非付费审批。
- PDF 的 `requestDesktopPdfPassword` 是普通私有回调：工作线程排队 UI 安全表单，返回值仅交加密核心，不成为 tool JSON、审批、question、历史或日志。它只取得新 PDF 的打开密码，双输入确认，非空有效 UTF-8、无 NUL、最多 127 字节，不 trim。UI 线程直接调用会失败；取消关闭本次表单，不关闭其他窗口。此路径与 MaiChat 登录密码不是同一提交 API。

## 已通过的构建和契约检查

- Desktop 主程序及第一批四个目标已编译成功。`change_password_tool` PASS（0.50s）、`maichat_host_tools_test` PASS（1.40s）、`login_dialog_test` PASS（42.94s），日志 `/private/tmp/maichat-password-desktop-ctest.log`。登录鉴权套件包括新增的严格布尔确认、近期登录过期和旧登录代次回复拒绝用例。
- 新 `desktop_password_change_test` 两个界面用例未通过，失败发生在打开表单前的应用前台门禁，不能记录为真实密码表单已经验收。后续只针对该独立目标调查，没有重跑 42 秒的完整鉴权套件或修改真实账号密码。

## 前台门禁失败的证据

- Qt 5.15.18 / Cocoa 的测试 dummy window 显示 `isActiveWindow=true`，但进程 `QApplication::applicationState=ApplicationInactive`，且没有其他 modal window。日志 `/private/tmp/maichat-password-desktop-dialog-diagnostic.log`。仅窗口级 active 不足以证明当前用户会话允许显示敏感表单。
- 只为测试添加公开 NSApp 前台激活 helper 后，仍是 ApplicationInactive；日志 `/private/tmp/maichat-password-desktop-dialog-activated.log`。未放宽生产 `canPresent` 的前台门禁，也未手工注入 Qt ApplicationActive。
- 隔离的 `/tmp` Qt+Cocoa probe 直接读取 native 状态：进入 app.exec 后 `ns_running=true`，但 `ns_active=false`，frontmost PID 仍是 855。普通 CLI 和临时 `.app` 经 LaunchServices 启动均如此；日志 `/private/tmp/maichat-native-activation-probe.log`、`/private/tmp/maichat-native-activation-app.log`。这排除了单纯“Qt 状态失真”或“没进入事件循环”的解释。
- 执行者进一步核对 PID 855 为系统 `loginwindow`；`ioreg` 的当前 IOConsoleUsers 会话报告 `CGSSessionScreenIsLocked=true`、onconsole=true、LoginDone=true。虽然外层 IOConsoleLocked=false，用户会话的明确 locked 状态仍阻止前台激活。根因是当前测试驱动运行在锁屏会话，生产表单拒绝显示符合预期；不是依靠重复启动测试可解决的产品故障。
- Qt 官方 [Cocoa delegate 实现](https://github.com/qt/qtbase/blob/5.15/src/plugins/platforms/cocoa/qcocoaapplicationdelegate.mm#L268) 在应用实际变为 active 时才发布应用状态变化；[event dispatcher 初始化](https://github.com/qt/qtbase/blob/5.15/src/plugins/platforms/cocoa/qcocoaeventdispatcher.mm#L593) 与应用是否处于用户前台是不同条件。本次结论主要依据 native 状态和会话 locked 的直接观测。

## 当前边界与后续单次验收

- 已停止锁屏下的 activation、临时 `.app` 和重复 Qt 界面测试尝试。现装 `/Applications/MaiChat.app`（当时 PID 13707）没有被停止或替换；未操作用户真实密码、手机或生产服务。
- 生产门禁继续要求 AppActive、可见窗口及无其他 modal。测试 helper 保留真实 NSApp 激活，并等待 ApplicationActive；解锁后再单次运行原生表单用例，不能用假 active 或跳过断言换取绿色结果。
- `MAICHAT_PASSWORD_FORM_CAPTURE` 可指定空白真实登录密码表单 widget PNG；它的 parent 是测试 dummy window，不是正常 MainWindow。锁屏调查期间尚未取得这个实际表单画面。PDF 私有表单的 worker/UI 确认与取消、正常 Settings/Agent 完整操作同样未做原生视觉验收。
- PDF prompt、AgentController 第二参数和 qpdf 许可打包已准备给统一构建；本记录不表示新 PDF 密码功能或 Desktop 新版本已经发布。
