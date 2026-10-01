# 进程内 FFmpeg / FFprobe 工具

MaiAgent 暴露 `ffmpeg` 和 `ffprobe` 两个工具。Desktop、iOS、Android 均调用仓库
`third_party/ffmpeg/fftools` 中同一份命令实现，作为静态库链接在 MaiChat 进程内；
不会启动子进程。独立的 MaiAgent 库不强制链接 FFmpeg，宿主提供执行和取消回调
后才注册这两个工具。

`ffprobe` 接收一个可访问的本地文件路径，返回 JSON 格式的容器和流信息：

```json
{"path":"/path/to/video.mp4"}
```

`ffmpeg` 接收 FFmpeg 参数数组，不包括程序名。例如提取一张视频帧：

```json
{"arguments":["-i","/path/to/video.mp4","-frames:v","1","/path/to/frame.png"]}
```

工具固定禁用交互式标准输入和覆盖已有输出；`-stdin`、`-y` 会被拒绝。
长任务通过现有 Agent 取消标志传给 FFmpeg 的中断回调。两个命令引擎共享原始
FFmpeg 的进程级状态，所以工具层串行调用，并在每次调用后恢复日志设置。
执行失败返回状态码，不调用 `exit()` 结束 MaiChat。`ffprobe` 的临时 JSON
结果会在读取后删除；超过 2 MiB 时明确标记截断。

媒体播放与 Graphics 原生视图属于 MaiChat `Media/`，不由这两个命令工具托管。
