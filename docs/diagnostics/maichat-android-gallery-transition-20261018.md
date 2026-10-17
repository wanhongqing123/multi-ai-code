# Android 图片连续浏览时的黑帧与窗口重建

- 报告背景：`MaiChat/AGENTS.md` 记录了 2026-10-14 的稳定回归：左右滑动媒体时先关闭旧预览、再建立新预览，只有 48px 淡入无法掩盖闪帧。本轮跨端对齐继续用 iOS 的单容器过渡作参照。
- 平台与时间：2026-10-18，Mac 上的 Android 17 `sdk_gphone16k_arm64` 模拟器 `emulator-5560`；源码基线 `c585ee1ed`，Debug APK 安装到该模拟器。
- 复现：从 Agent 媒体序列打开红色图片，向左滑至蓝色图片，再向右返回。旧代码 `MainActivity.showFullScreenImage()` 每次先 `imageDialog.dismiss()`，然后 `new Dialog()`；窗口生命周期证明旧帧无法保留在同一容器。
- 修复：`09d3ebf41c23d96ceee555c36e144edd7b00efba`。相邻图片复用同一个 Dialog；切换前从当前 Graphics TextureView 取快照，新图片呈现前保留快照，新图片就绪后双方在同一 FrameLayout 内完成 220ms 过渡。旧图片的异步失败回调不能关闭新预览；新图片失败时回到前一张。普通图片、视频入口仍复用各自已有的代码调用入口和 Agent 工具入口。
- 验证：红图→蓝图→红图的设备测试确认 Dialog 对象不变、两张图都实际显示；原有图片→视频→图片测试由一次失败定位到过早禁用图片触摸后修复，再次通过。Android 相关 9 项设备测试及 Debug 单元测试通过。录屏 `/tmp/maichat-gallery-transition2.mp4` 的中心像素采样在图片段依次为红、蓝、红，三段之间未采到黑帧；截图 `/tmp/mai-android-image-gallery-continuous.png` 显示蓝图及控制按钮。
- 验证边界：该录屏约 20 秒、71 个编码帧，不能证明显示器的每一帧；Android 实体机、图片与视频之间的转场及大图快速连滑尚未实机验收。现有图片与视频入口仍需继续对齐 iOS 的转场行为。
