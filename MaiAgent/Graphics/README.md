# MaiAgent Graphics 设计草案

状态：已纳入 OBS Graphics 原始源码子集；GPU 后端尚未接入 MaiAgent，播放器尚未实现。参考 OBS Studio 主干提交
[`50530ce9`](https://github.com/obsproject/obs-studio/tree/50530ce9046599e698c5d2068e4f053fae2318f6)
的 Graphics 分层与 Effect 语法。原始源码按目录保存在
[`obs-studio/`](obs-studio/UPSTREAM.md)，并保留版权及许可文件；现有 `ag_`
适配层仍是实验实现，尚未改为调用 OBS 后端。

当前代码位于 `include/MaiGraphics.h`、`src/MaiGraphics.cpp`、
`include/MaiGraphicsTaskRunner.h`、`src/MaiGraphicsTaskRunner.cpp` 和
`include/MaiGraphicsEffectParser.h`、`src/MaiGraphicsEffectParser.cpp`。
构建时显式启用 `MAIAGENT_BUILD_GRAPHICS=ON`。已有 `ag_` 离屏 RGBA8
软件参考后端、固定物理线程 TaskRunner，以及识别 Effect `technique/pass`
结构的独立解析器。尚无 GPU 着色器编译、硬件帧导入、声音或窗口呈现；
实验性接口在这些能力接入前不承诺稳定 ABI。

## 目标与边界

- 消息区、AI 助手区和 Agent 图片处理复用同一套帧、效果、合成与呈现规则。
- Graphics 管理设备、纹理、渲染目标、Effect、图层合成和屏幕呈现。FFmpeg
  的解封装、解码、音频时钟及播放控制属于独立的 Media 层；两层通过视频帧契约连接。
- 公共 C 接口统一使用 `ag_` 前缀（Agent Graphics Subsystem）；不向公共头暴露
  Qt、Swift、Java、`AVFrame` 或特定图形 API 类型。内部 C++ 实现仍使用 `Mai`
  前缀和 MaiAgent 编码规范。
- 首批后端：Apple Metal（iOS、macOS 共用渲染核心，各自接入界面表面）、
  Windows D3D11、Android OpenGL ES。桌面 OpenGL 可作为后续兼容后端。
  D3D12 不在首批范围。
- Graphics 作为 MaiAgent 的可选独立构建目标，避免未使用图形功能的纯 Agent
  嵌入方被迫链接平台图形库。

## 分层

```text
MaiChat 消息区 / AI 助手区 / Agent 图像工具
                  │
         异步宿主桥接与播放控制
                  │
     MaiAgent Media：FFmpeg 解码、时钟、队列
                  │ 视频帧及所有权契约
     MaiAgent Graphics：ag_ API、Effect、合成
                  │ 后端能力与资源接口
       Metal / D3D11 / OpenGL ES
                  │
       系统窗口表面与音频输出适配
```

OBS 的 [`graphics.h`](https://github.com/obsproject/obs-studio/blob/50530ce9046599e698c5d2068e4f053fae2318f6/libobs/graphics/graphics.h)
公开不透明资源句柄；[`device-exports.h`](https://github.com/obsproject/obs-studio/blob/50530ce9046599e698c5d2068e4f053fae2318f6/libobs/graphics/device-exports.h)
定义后端职责。MaiAgent 沿用这一边界，但首版 API 只包含媒体播放和图像处理
实际需要的资源与操作；每项可选能力须可查询，不能靠调用失败猜测后端支持情况。

## 公共资源与帧契约

公共 C 接口使用不透明的 `ag_graphics_t`、`ag_surface_t`、`ag_texture_t`、
`ag_render_target_t` 和 `ag_effect_t`。首批操作分为设备/能力查询、表面附着与
重建、纹理创建与导入、离屏渲染、Effect 编译与参数设置、合成、呈现和销毁。
函数命名沿用 OBS 的职责划分。当前实验性接口有 `ag_create`、
`ag_destroy`、`ag_texture_create`、`ag_draw_sprite` 等；
`ag_effect_create`、`ag_present` 待 GPU 后端与宿主表面契约确定后加入。
所有资源由创建它的图形设备持有；销毁必须回到该设备的图形线程。设备丢失、
应用进入后台或表面重建后，旧 GPU 资源句柄不得继续使用。

视频帧包含时间戳、尺寸、像素格式、平面与行跨度、色彩原色/传递函数/矩阵/
范围、旋转和显示宽高比。硬解帧通过后端专用导入描述传递原生资源句柄、
同步栅栏及释放回调；提交后由明确的引用所有权保证帧在 GPU 使用结束前存活。
软件帧可走 CPU 平面上传，作为所有后端的回退路径。画面转换不能默认先复制为
RGBA，否则会丢失零拷贝路径及 HDR 信息。

## 图形线程

首版每个 Graphics 设备由一个专用图形线程持有，设备创建、GPU 资源创建与
销毁、渲染命令及呈现都在这条线程执行。宿主和 Media 层通过 TaskRunner 投递
任务与异步结果，不在 UI 线程同步等待 GPU。FFmpeg 解封装/解码、磁盘读取和
Effect 词法解析仍可在其他工作线程执行；解析后的着色器编译和 GPU 资源创建
必须回到图形线程。表面与应用生命周期通知由各平台 UI 线程转交。

这里借鉴 Chromium 的
[`SingleThreadTaskRunner`](https://chromium.googlesource.com/chromium/src/+/39dbf8ad204a573f3757d6dee260c7d4d24c4c00/docs/threading_and_tasks.md)：
普通 `SequencedTaskRunner` 只保证顺序，不保证同一物理线程；Graphics 设备
要求后者。MaiAgent 内部可增加 `MaiGraphicsTaskRunner`，提供任务投递、
当前线程断言、停止接收、排空或取消待执行任务，以及有序销毁。不能用一个
全局锁代替线程归属。OBS 的
[`gs_enter_context`/`gs_leave_context`](https://github.com/obsproject/obs-studio/blob/50530ce9046599e698c5d2068e4f053fae2318f6/libobs/graphics/graphics.c)
通过线程局部上下文与互斥锁序列化调用；MaiAgent 首版选择固定图形线程，
减少跨线程迁移上下文的状态面。

## Effect

保持 OBS `.effect` 的核心概念：参数、sampler、函数、`technique` 与 `pass`，
一个 pass 指定顶点和像素着色器入口。实现拆成三步：

1. 独立的词法/语法解析器生成带源码位置的 AST，并验证参数和 pass 引用。
2. 整理每个 pass 依赖的函数、结构和参数，生成后端中间表示；语法错误返回
   文件、行列和可操作的错误信息。
3. 各后端分别编译、缓存着色器与管线状态；同一 Effect 在不同设备上拥有
   独立的 GPU 资源。后台解析不直接触碰 GPU。

OBS 的
[`effect-parser.h`](https://github.com/obsproject/obs-studio/blob/50530ce9046599e698c5d2068e4f053fae2318f6/libobs/graphics/effect-parser.h)
明确包含“按 technique/pass 生成各自着色器”的职责，其实现还依赖
`cf-parser`、`shader-parser` 与 Graphics 的着色器创建接口。因此迁移语法
必须连同生成、编译和报错契约验证，不能只解析出 Effect 名称。当前解析
测试使用仓库自有的最小 Effect 样例；后续还需以 OBS 自带 Effect 文件作为
行为参考，验证预处理、shader 生成和编译结果。不复制其解析器源码。

## 后端与验收顺序

- Metal：渲染核心共享。iOS 的 `UIView` 与 macOS 的 `NSView` 分别承载
  `MTKView`/`CAMetalLayer`；表面尺寸、前后台切换和 GPU 家族能力在宿主适配。
  [OBS 的 Metal 实现](https://github.com/obsproject/obs-studio/blob/50530ce9046599e698c5d2068e4f053fae2318f6/libobs-metal/README.md)
  目前只覆盖 Apple Silicon Mac，不能直接充当 iOS 后端。
- D3D11：接入 Windows 表面与设备丢失重建；先验证上传 NV12/RGBA、
  离屏合成和呈现。
- OpenGL ES：为 Android 单独实现表面和纹理导入；OBS 桌面 OpenGL 后端
  仅作为抽象参考。
- 每个后端先通过相同的离屏图像比较、线程归属、资源销毁、表面重建和
  Effect pass 测试，再接消息区及 AI 助手播放器。实际播放还需验证音画同步、
  暂停/跳转、旋转、色彩及后台恢复。

当前软件参考后端仅用于锁定 API 与线程/资源契约，不会替换三端现有播放器。
