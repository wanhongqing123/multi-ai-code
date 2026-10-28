# iOS 云盘左滑删除与图片、视频连续浏览

- 时间：2026-10-28；源码基线 `124daca3b2032afc31953da12bb1fea33d020e4d`。
- 用户最终要求：文件行左滑，在右侧揭露删除按钮，点击后确认；打开任意云盘图片或视频后，左右滑按云盘原顺序浏览，下一项是视频时可以播放。

## 原因与修正

- 旧 `MaiCloudDriveSwipeRow` 使用 `.leading` 和正向位移，只能右滑后在左侧露出删除。现在使用 `.trailing`、`[-84, 0]` 的位移与负阈值；右滑收起。完整左滑只揭露按钮，不直接删除。纵向滚动和原确认流程继续复用。
- 旧 `MaiCloudDriveView.open` 用 `model.videos.filter(\.isImage)` 构造预览序列；视频另走单个 `MaiFfplayVideoScreen`，所以图片浏览会跳过视频。现在图片、视频共用 `MaiCloudDriveMediaPager`，保留原列表顺序，分页仅按 ID 去重追加。
- 当前页为视频时才通过普通 `MaiCloudDriveClient.openVod` 建立共享 C++ Range 点播并嵌入现有 FFplay 控制器；预取的邻页不启动音频或播放器。切走、关闭时先停止播放线程，再移除子控制器并取消、释放 VOD；返回视频重新获取来源。进度条、倍速按钮触摸期间停用翻页，图片放大期间仍只平移图片。
- 旧全局播放器必须在新视频视图挂窗之前停止，避免 RootView 常驻视频 dock 与新播放实例短暂重叠。`stop` 只清理属于自己的 active 指针。详细播放状态仅在显式 UI 测试模式输出，正常 VoiceOver 使用中文状态，不读内部 ID 或调试字段。
- 首帧状态来自 `MaiFfplayIosBridge` 中图形 presenter 成功接收的视频帧，既不靠计时，也不把进入播放器当作实际播放成功。

## 验证证据

- 已用旧二进制录制右滑/左按钮基线；修后图片与视频删除用例 2/2 通过，并查看右侧红色按钮截图。结果 `/private/tmp/maichat-cloud-left-delete-final.xcresult`；修后录像 `/private/tmp/maichat-cloud-left-delete-final.mp4`。
- iOS 26.5 隔离模拟器最终相关 6/6 通过：混合浏览两项、图片缩放/失败重试/首尾边界、分页追加、图片左滑确认取消及删除、原视频播放/显式保存/左滑删除。结果 `/private/tmp/maichat-cloud-mixed-media-final.xcresult`。随后只强化测试 active 匹配为完整字段，排除 `active=0` 误匹配 `owned_active=0` 的假阳性；产品代码不变，混合两项再次 2/2 通过，零跳过，结果 `/private/tmp/maichat-cloud-mixed-media-precise-active.xcresult`。
- 混合测试通过普通 Swift 上传安全视频，实际走本地 TLS 的共享 VOD Range。走通图片→视频实际首帧、暂停、拖进度条不翻页、滑到图片后全局播放器为空、回视频建立第二次来源、关闭后停止并清理。也从视频初始页验证两侧浏览。桥接首帧 Debug/Release 回归通过，零帧会话不报告 ready。
- iPhone 14 Pro Max、iOS 27.2、正常 App 真实账号只读验收 2/2、零跳过（82.01 秒）：右滑不露删除、左滑按钮在右侧、图片下载解码显示；云盘第 23/26 项 MOV 显示真实画面，暂停/恢复、滑到相邻媒体、滑回视频重新播放均通过。root 已亲看完整图片、右侧删除与真实视频画面；没有上传、移动或删除真实账号文件。结果 `/private/tmp/maichat-cloud-media-device-left-final.xcresult`，截图位于同名 `-att` 目录。
- 已安装并重新打开的 App 为 `com.kongshang.maichat`，2.2.4 (131) Debug；签名严格校验通过，重新启动的进程 1824 指向新容器 `/private/var/containers/Bundle/Application/3DA70D21-3A44-4676-B5B8-280980D280F6/MaiChat.app/MaiChat`。`MaiChat.debug.dylib` SHA-256：`41da07ca16802a086fcda7b5546b1e8e37bf59ea33c3bb158fb7441b1c526f2d`。
- 真机结果有既有同步音频会话 API 在主线程调用的运行时警告；本次未观察到用例超时或卡住，未改音频会话实现。此次范围为 iOS 界面与播放桥接，不代表其他平台混合预览已完成实机验收。
