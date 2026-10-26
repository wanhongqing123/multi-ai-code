# iOS 与 Desktop Agent 摄像头工具关闭与非阻塞控制

- 现场：2026-10-23。用户指出 iOS 和 PC 的摄像头 Agent 工具没有独立关闭能力，并质疑打开摄像头为何会卡住工具。
- 原因：旧版 `mobile_take_photo` / `maichat_take_photo` 将打开预览、等待用户按快门、保存 JPEG 合在一次同步工具调用里。工具直到用户拍照或取消才返回，同一 Agent 轮次无法再调用关闭工具。iOS 超时清除了等待但可能留下预览；Desktop Agent 停止标志此前也不会关闭等待中的 Qt 弹窗。
- 改动：iOS 与 Desktop 的 Agent 正式入口统一为 `maichat_camera_command`，操作为 `open`、`status`、`close`。`open` 展示摄像头后立即返回；用户仍亲自按系统或 Qt 取景器快门，JPEG 写入当前 Agent Workspace；`status` 返回打开状态及当前 Session 已拍摄照片的相对路径，`close` 关闭预览。预览与照片按 Session 隔离，另一 Session 不能接管或关闭。iOS 原 `mobile_take_photo` 不再向模型暴露；Android 仍使用平台自己的 `mobile_take_photo`，这轮未重做 Android 系统相机入口。普通界面的拍照继续复用既有取景器实现。
- 生命周期：iOS 关闭、取消、停用页面、账号切换及最长 10 分钟到期均清除请求，撤下系统相机；异步授权回来前若请求已取消，不再弹出预览。Desktop 关闭时拒绝弹窗、停止 `QCamera`、清理未完成照片；普通界面拍照仍使用其取消键，原捕获 API 也支持调用方传入取消标志。Desktop 连续关闭再打开时，旧窗口销毁回调按预览标识检查，避免清除新窗口状态。
- 验证：Mac 正常 Cocoa 窗口中实体摄像头工具 `open` 很快返回，预览保持打开且拍照键变为可用；跨 Session 打开/关闭被拒绝，所属 Session 连续关闭、打开、关闭通过且无 JPEG 残留。`QWidget::grab()` 截图可见取景器窗口的关闭和拍照控件，但没有捕获原生相机画面，未据此声称画面内容已验收。旧同步拍照入口的取消测试也通过。iOS 26.4 模拟器正常 App 通过本地假模型调用 `open`，预览仍在时由同账号 Mac 测试端向其 Session 发送“camera-close”，Agent 调用 `close` 后系统相机撤下，再在本机调用 `status`；三次工具请求均走完且没有等用户拍照。已亲看打开与关闭截图；关闭图中本地假账号有 IM 70402 提示，与相机命令无关。Android `:app:assembleDebug` 成功，iOS 真机当前不可用；真机硬件取景、拍照文件与长期后台恢复尚未验证。
- 构建边界：本地 Mac/iOS 源码产物通过，用户正在运行的安装版尚未覆盖；没有发布新包。
