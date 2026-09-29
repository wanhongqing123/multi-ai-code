# MaiAgent Graphics

本目录保留从 OBS Studio 提取的 Graphics 源码及构建验证。源码快照的版本、
范围和许可见 [`UPSTREAM.md`](UPSTREAM.md)，SIMDe 位于
[`../third_party/simde/`](../third_party/simde/README.md)。

原先自行实现的软件渲染接口、Effect 简化解析器和图形线程类已移除。
当前没有 MaiAgent 对外 Graphics API，也没有可用的 GPU 后端或播放器。

启用 `MAIAGENT_BUILD_GRAPHICS=ON` 后，构建目标
`maiagent_obs_graphics_check` 会编译复制的 OBS Graphics C 源码：

- 10 个向量、矩阵等源文件构成 `maiagent_obs_math`；
  `MaiObsMathTests` 在 ARM64 上运行 OBS 的变换和逆变换，覆盖 SIMDe 路径。
- 7 个 Graphics 核心源文件作为编译检查；它们尚未链接成 libobs 运行时。
- macOS 上还用仓库内 FFmpeg 源码生成配置头，编译第 18 个
  `graphics-ffmpeg.c`。这里只编译，不链接 FFmpeg 库或执行图像加载。
- macOS 上的 `maiagent_obs_util` 链接 OBS 的词法器及依赖，
  `MaiObsLexerTests` 检查注释、续行和错误输入。

这些检查不代表 Metal、D3D11、OpenGL 后端已经运行。要完成 Graphics
集成，仍需解决 OBS Graphics 核心与 util、FFmpeg、各后端的运行时链接，
再实现宿主窗口和媒体帧的接入。
