# 全平台视频超分调研（2026-10-05）

结论：可以做成 `video_super_resolution` 共享工具，但首版应以 **2×、短视频、
明确的 SDR 输入**做真机性能和画质验证，再开放 4×及 HDR。逐帧模型与真正的视频
时序模型是两种不同技术，不能在工具能力清单里混称。

| 路线 | 能力与成熟度 | 全平台部署判断 |
|---|---|---|
| FFmpeg Lanczos | 传统插值，速度快、可作基线；不会凭空恢复丢失的纹理 | 现有 FFmpeg 已覆盖四端，但不应宣传为 AI 超分 |
| Real-ESRGAN + ncnn | 官方有逐帧视频脚本和 ncnn/Vulkan 模型；可用分块推理控制显存。每帧独立处理，运动区域需要检查闪烁 | ncnn 官方覆盖 Android、iOS、macOS、Windows；iOS GPU 路径需 MoltenVK，具体性能必须测量 [Real-ESRGAN 视频实现](https://github.com/xinntao/Real-ESRGAN/blob/master/inference_realesrgan_video.py)、[ncnn 平台说明](https://github.com/Tencent/ncnn/wiki/FAQ-ncnn-vulkan) |
| BasicVSR++ / RealBasicVSR | 利用相邻帧的传播与对齐，属于真正的视频超分；RealBasicVSR 还处理真实世界压缩和噪声 | 官方实现主要是 PyTorch/CUDA，直接移到四端成本和算力要求高，先作为桌面／服务端质量对照 [BasicVSR++](https://github.com/open-mmlab/mmagic/blob/main/configs/basicvsr_pp/README.md)、[RealBasicVSR](https://github.com/open-mmlab/mmagic/blob/main/configs/real_basicvsr/README.md) |
| SeedVR2 | 高质量生成式视频修复 | 官方示例称 100 帧 720p 需单张 H100 80GB；不适合作为手机本地首版 [官方仓库](https://github.com/ByteDance-Seed/SeedVR) |

推荐做一个**共享工具契约**，而不是四端各写一套算法：

```text
video_super_resolution(
  input_path,
  output_path,
  scale: 2 | 4,
  profile: fast | temporal,
  tile_size?,
  preserve_audio: true
) -> 新视频路径、原/新尺寸、帧数、处理时长、所用模型与后端
```

`fast` 的候选是 Real-ESRGAN 的逐帧推理；`temporal` 只有在时序模型于目标设备
通过性能和画质测试后才上报可用。共享 C++ 层负责 FFmpeg 解码、分块重叠、模型调用、
拼回、编码、音频复制和任务进度。模型推理后端可比较 ncnn/Vulkan 与仓库已有的
ONNX Runtime：后者官方支持 iOS CoreML、Android NNAPI，并提醒算子拆分会影响性能
([ONNX Runtime 移动端](https://onnxruntime.ai/docs/tutorials/mobile/)、
[CoreML EP](https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html)、
[NNAPI EP](https://onnxruntime.ai/docs/execution-providers/NNAPI-ExecutionProvider.html))。

媒体保真是首版门槛。先用 `ffprobe` 读取旋转信息、帧率、时间戳、像素宽高比、
色彩 primaries/transfer/matrix/range、位深和 HDR 元数据；输出要保留音轨与时间线。
通用的 8-bit RGB 超分不能静默处理 10-bit HLG/PQ/Dolby Vision；未实现正确的
HDR 路径前，工具应明确拒绝这些输入，或让用户明确选择高质量 SDR tone-map。
AI 超分会推测细节，真人脸、文字和衣纹要重点检查失真与帧间闪烁。

建议先拿同一批实拍视频做四端基准：480p→960p 和 720p→1440p，各取 10 秒、
30 fps，覆盖静态、快速运动、真人脸、细文字和压缩噪声。记录每秒处理帧数、
峰值内存、耗电／温度、成片大小、音画同步和视觉对比；同时用 Lanczos 产物作基线。
首轮通过后再决定 4×、长视频任务恢复和时序模型。ncnn 使用 BSD-3-Clause，
Real-ESRGAN ncnn 实现使用 MIT；引入模型文件时仍需核对对应权重的发布许可
([ncnn 许可](https://github.com/Tencent/ncnn/blob/master/LICENSE.txt)、
[Real-ESRGAN ncnn 许可](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/blob/master/LICENSE))。
