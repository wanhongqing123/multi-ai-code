# MaiChat 拍照与本地音乐（2026-10-22）

## 用户操作

- 拍照：iOS、Android、Desktop 的创作中心输入区可打开系统摄像头预览；用户按快门后得到工作区的新图片，再决定是否发送给 Agent。取消、无摄像头、权限拒绝和拍摄失败均保留输入草稿，不生成空附件。
- 本地音乐：用户通过系统文件选择器查找设备上的 MP3、M4A 或 WAV 文件，在 MaiChat 内播放、暂停、拖动进度和停止；切换文件时上一首停止。选择器不遍历未授权目录，也不自动上传音乐。首版不接 Apple Music、在线曲库或受 DRM 保护的媒体。
- Agent 工具与普通按钮使用同一拍摄／播放实现。工具不得跳过操作系统授权或模拟用户按快门；打开摄像头和播放声音的工具应经每次调用确认。Agent 可读取用户明确选中的文件，不得全盘扫描音乐目录。

## 实现

- iOS、Android 创作中心复用现有拍照入口；Desktop 用 Qt 摄像头预览与快门。三端的 `mobile_take_photo`／`maichat_take_photo` Agent 工具都等待用户操作，拍摄后保存到当前工作目录。
- iOS 使用 Files 选择器与 AVAudioPlayer，Android 使用系统文档选择器与 MediaPlayer，Desktop 使用本地文件选择器与 QMediaPlayer。三端 `+` 菜单和 Agent 工具复用播放器，支持 MP3、M4A、WAV 的播放、暂停、进度调整和停止。
- iOS、Android 工具名为 `mobile_play_local_music`，Desktop 为 `maichat_play_local_music`。无路径时由用户选择文件；指定路径时只接受 Agent 工作目录内的文件。

## 验收边界

1. 各端普通入口、Agent 工具入口分别测试选择／授权／取消／失败／成功；拍照和音乐播放不得自动发送 IM 或触发模型付费任务。
2. 文件不可读或格式不可解码时显示错误；播放器关闭或 App 销毁时释放资源。拍照保存失败不能返回不存在的路径。
3. iOS 模拟器可验证无摄像头和音乐文件流程，但真正拍照要用真机；Android 模拟器与 Desktop Mac 实测，Windows 在 Windows 环境实测。未跑的平台明确标为未验证。

## 本轮已跑的验证

- iOS：模拟器普通音乐入口的 Files 选择器与播放器 UI 测试通过；arm64 Release 无签名构建通过。真机因连接不可用，当前改动尚未完成真机相机和音乐验收。
- Android：16 KB 页面模拟器的普通拍照、系统音乐选择器和播放器画面已查看；本地 WAV 播放控制仪器测试通过。Agent 发起选择器的完整链路尚未实机走通。
- Desktop Mac：Release 构建、音乐播放器与相关 Agent 面板定向测试通过，暂存 App `--smoke` 通过；物理摄像头快门和 Windows 运行未验证。
