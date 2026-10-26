# MaiChat 媒体适配

渲染部分遵循 [`MaiAgent/Graphics/README.md`](../../MaiAgent/Graphics/README.md)
中的 OBS 参照规范。播放器所需的纹理上传、色彩转换和绘制顺序先对照 OBS
`libobs/obs-source.c` 与 `libobs/data`；平台差异以实际画面或像素验证。

`MaiGraphicsPresenter` 管理 MaiChat 原生视图、Graphics 线程、交换链和绘制 effect；
它调用 MaiAgent 移植的 OBS `gs_*` 图像引擎，不属于引擎本身。

`MaiVideoPlayback` 管理一个视频文件的 FFmpeg 读流、解码、播放时钟及
播放/暂停/跳转/逐帧/倍速/循环命令，向 Presenter 提交画面。目前没有音频输出，
不能当作完整音视频播放器使用。播放器对齐 ffplay 时，解码、时钟与命令状态留在
媒体层；像素格式转换和画面缩放交给 Graphics 的纹理及 effect。

两者的 C 接口供 Desktop、iOS 和 Android 的 MaiChat 视图使用。MaiAgent 工具
若要控制播放，应通过 MaiChat 提供的媒体会话接口下发命令，不直接持有原生视图。

## 视频圈发布前的 AI 标记

`MaiVideoCircleMark.h` 提供 Desktop、iOS、Android 共用的 C 接口。宿主把已完成生成任务的
本地 MP4 与一个尚不存在的绝对目标路径交给工作线程；核心用内嵌 FFmpeg 将
`data/ai_generated_badge.png` 的“AI 生成”标牌叠到每帧左上角，并写入 MP4 的
`mai_ai_generated=1`、`mai_ai_label=AI生成` 和公开说明元数据。内嵌 FFmpeg
没有 `drawtext` 滤镜，因此徽标的 PNG 字节同时编译在
`data/ai_generated_badge.inc` 中，不依赖各平台字体或外部 `ffmpeg` 程序。

核心清除来源文件的容器及流元数据，复制可用音轨，把画面转为 H.264/yuv420p。
输出只在复核时长、文件元数据及首帧中的蓝底和白字像素后原子提交；原文件始终不变。
返回输出字节数、时长和 SHA-256。取消、转码失败及大小/时长越界只清理这次调用独占
创建的临时文件。大小上限为十进制 200 MB，时长上限为 60 秒，与服务端发布约束一致。

标牌与元数据只能说明发布文件经过了标记流程，不能独立证明素材确由 AI 生成。
发布入口还须核对同一会话中已完成的生成任务来源，公开前仍须人工审核。
内嵌 FFmpeg 命令与 Agent 的 FFmpeg 工具共用 `maiRunFfmpegSerialized` 进程锁，
不能绕过它直接并发调用 `mai_ffmpeg_execute`。
