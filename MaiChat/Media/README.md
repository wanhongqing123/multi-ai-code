# MaiChat 媒体适配

`MaiGraphicsPresenter` 管理 MaiChat 原生视图、Graphics 线程、交换链和绘制 effect；
它调用 MaiAgent 移植的 OBS `gs_*` 图像引擎，不属于引擎本身。

`MaiVideoPlayback` 管理一个视频文件的 FFmpeg 读流、解码、播放时钟及
播放/暂停/跳转/逐帧/倍速/循环命令，向 Presenter 提交画面。目前没有音频输出，
不能当作完整音视频播放器使用。播放器对齐 ffplay 时，解码、时钟与命令状态留在
媒体层；像素格式转换和画面缩放交给 Graphics 的纹理及 effect。

两者的 C 接口供 Desktop、iOS 和 Android 的 MaiChat 视图使用。MaiAgent 工具
若要控制播放，应通过 MaiChat 提供的媒体会话接口下发命令，不直接持有原生视图。
