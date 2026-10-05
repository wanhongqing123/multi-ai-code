# 进程内 FFmpeg / FFprobe 工具

MaiAgent 暴露 `ffmpeg` 和 `ffprobe` 两个工具。Desktop、iOS、Android 均调用仓库
`third_party/ffmpeg/fftools` 中同一份命令实现，作为静态库链接在 MaiChat 进程内；
不会启动子进程。独立的 MaiAgent 库不强制链接 FFmpeg，宿主提供执行和取消回调
后才注册这两个工具。

`ffprobe` 接收一个可访问的本地文件路径，返回 JSON 格式的容器和流信息：

```json
{"path":"/path/to/video.mp4"}
```

默认 `metadata` 模式还返回章节；需要时可设置 `include_programs`、`count_frames` 或
`select_streams`（如 `v:0`、`a:0`）。`count_frames` 会读取整段媒体，长视频耗时较长。
分析关键帧可指定有界区间：

```json
{"path":"/path/to/video.mp4","mode":"keyframes","start_s":0,"duration_s":30}
```

关键帧模式返回各帧的时间、类型和尺寸，时长上限 120 秒。视频旋转角度、色彩空间、
primaries、transfer、像素格式、HDR side data 等在源文件有标记时由默认
`-show_streams` JSON 原样给出；FFprobe 不会凭空补齐缺失的元数据。
同一接口还可设 `mode="frames"` 或 `mode="packets"`，读取一个最多 30 秒的区间：
前者返回解码帧时间、类型、关键帧标记和尺寸，后者返回包的 PTS/DTS、时长、大小与
关键帧标志。默认只查 `v:0`，可用 `select_streams="a:0"` 查看首条音轨的帧或包。
数据超过 2 MiB 时会报错并提示缩短区间，保证回给模型的 JSON 是完整的。

`ffmpeg` 接收 FFmpeg 参数数组，不包括程序名。例如提取一张视频帧：

```json
{"arguments":["-i","/path/to/video.mp4","-frames:v","1","/path/to/frame.png"]}
```

用 FFmpeg 分析滤镜时可显式请求成功日志，例如音量扫描：

```json
{"arguments":["-i","/path/to/video.mp4","-af","volumedetect","-f","null","-"],"capture_log":true}
```

此时结果含 `status`、最多 16 KiB 的 `log_tail` 及 `log_truncated`。它适用于
`volumedetect`、`silencedetect`、`blackdetect` 等把分析结果写到日志的滤镜。
默认转码仍只返回完成状态，避免把编码过程日志塞进模型上下文。若日志被截断，
工具会如实标记，Agent 应缩短分析区间，不可把尾部当全量结果。

工具固定禁用交互式标准输入和覆盖已有输出；`-stdin`、`-y` 会被拒绝。
长任务通过现有 Agent 取消标志传给 FFmpeg 的中断回调。两个命令引擎共享原始
FFmpeg 的进程级状态，所以工具层串行调用，并在每次调用后恢复日志设置。
执行失败返回状态码，不调用 `exit()` 结束 MaiChat。`ffprobe` 的临时 JSON
结果会在读取后删除；超过 2 MiB 时返回明确错误，不能把截断的不完整 JSON
回灌给主模型。

媒体播放与 Graphics 原生视图属于 MaiChat `Media/`，不由这两个命令工具托管。

## 人物保真比例调整

`video_stretch_lower` 是 `ffmpeg` 之上的固定配方，不调用付费生成模型：

```json
{"input_path":"person.mp4","output_path":"person-taller.mp4","split_y":340,"factor":1.12}
```

工具先调用 `ffprobe` 校验源文件，只接受偶数尺寸的 8-bit `yuv420p` SDR，且色彩
标记为 BT.709 或未标记；P3/HDR、10-bit 输入会明确拒绝，避免悄悄把色域或动态范围
压掉。`split_y` 必须为偶数，且在画面内部。固定滤镜将上方区域原样裁出，只对下方
区域纵向拉伸，再拼接成偶数高度。输出使用 H.264 `-qp 0` 无损编码、BT.709 VUI，
音频尽量流复制；输出文件可能显著增大。实测 640×360、分界线 180、系数 1.12，
输出为 640×380，解码后的上方 180 行与输入逐帧 SHA-256 完全一致；真实素材仍应
在交付前检查分界线和人体比例。独立 C++ 入口与模型工具复用同一实现，实际内嵌
FFmpeg 测试在 `MaiEmbeddedFfmpegToolsTest` 覆盖。

`MaiEmbeddedFfmpegToolsTest` 在 macOS/Windows Desktop、Android 和 iOS 模拟器
共用同一份测试代码，覆盖失败后的再次调用、重复转换和真实取消。移动端测试构建
需开启 `MAICHAT_MOBILE_TESTS`；单独运行 iOS 工具测试时可设置
`MAICHAT_IOS_METAL_RENDER_PROBE=OFF`，避免同时构建不相关的混合 Swift/C
渲染探针。
