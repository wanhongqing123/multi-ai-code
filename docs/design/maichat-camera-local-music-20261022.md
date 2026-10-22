# MaiChat 拍照与音乐（2026-10-22）

## 用户操作

- 拍照：iOS、Android、Desktop 的创作中心输入区可打开系统摄像头预览；用户按快门后得到工作区的新图片，再决定是否发送给 Agent。取消、无摄像头、权限拒绝和拍摄失败均保留输入草稿，不生成空附件。
- 音乐：用户可选择 MP3、M4A 或 WAV 文件；iOS 另可从系统音乐库选择包括 Apple Music 订阅在内的歌曲，Android 可搜索系统媒体库。播放界面支持暂停、进度调整、停止和重播；切换歌曲时上一首停止。iOS、Android 返回聊天后保留小播放器入口，可重新打开控制面板。选择器不自动上传音乐。
- Agent 工具与普通按钮使用同一拍摄／播放实现。打开摄像头不重复弹出 Agent 批准：系统权限仍按操作系统规则授权，用户仍须在相机预览里按快门。音乐播放工具继续逐次确认；Agent 可读取用户明确选中的文件，不得全盘扫描音乐目录。

## 实现

- iOS、Android 创作中心复用现有拍照入口；Desktop 用 Qt 摄像头预览与快门。三端的 `mobile_take_photo`／`maichat_take_photo` Agent 工具都等待用户操作，拍摄后保存到当前工作目录。
- 三端共享 `MaiChat/Media/FfplayCompat/MaiFfplayAudioSession`：用 FFplay 的 `-nodisp` 模式解码、输出和跟踪时间轴。桌面、iOS、Android 只保留本地选择器、权限和界面适配；共享核心处理无画面播放时的暂停、跳转与停止。
- iOS 的系统音乐库由 MediaPlayer 选择；可导出的无保护歌曲先导出到 App 临时目录，再交给 FFplay。Apple Music 受保护或云端歌曲由 Apple 官方 `MPMusicPlayerController.applicationQueuePlayer` 播放，沿用同一个 MaiChat 播放界面，不尝试提取受保护音频。Android 使用 `MediaStore.Audio` 查询用户授权的设备音乐，选中后复制到应用缓存再交给 FFplay。Desktop 默认打开系统音乐目录的文件选择器。
- iOS、Android 工具名为 `mobile_play_local_music`，Desktop 为 `maichat_play_local_music`。无路径时用户选择系统音乐库或文件；指定路径时只接受 Agent 工作目录内的文件。

## 验收边界

1. 各端普通入口、Agent 工具入口分别测试选择／授权／取消／失败／成功；拍照和音乐播放不得自动发送 IM 或触发模型付费任务。
2. 文件不可读或格式不可解码时显示错误；播放器关闭或 App 销毁时释放资源。拍照保存失败不能返回不存在的路径。
3. iOS 模拟器可验证无摄像头和音乐文件流程，但真正拍照要用真机；Android 模拟器与 Desktop Mac 实测，Windows 在 Windows 环境实测。未跑的平台明确标为未验证。

## 本轮已跑的验证

- iOS：模拟器上来源选择器和 FFplay 音乐播放器 UI 测试通过；arm64 Release 无签名构建通过。该模拟器打开系统音乐选择器时提示缺少系统 App 扩展，无法用于验收 Apple Music；真实订阅曲目和资料库导出需在有内容的 iPhone 上验收。
- Android：16 KB 页面模拟器上系统音乐权限、曲目搜索列表、选歌与 FFplay 播放画面已查看；WAV 播放、暂停和继续的仪器测试通过。Agent 发起选择器的完整链路尚未在真实账号中走通。
- Desktop Mac：共享 FFplay 音频实际 WAV 播放、暂停、跳转、停止探针通过；音乐窗口与视频播放器定向测试通过，窗口截图已查看。Windows 运行未验证。
