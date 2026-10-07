# 视频模型工具字段核对与实测

核对日期：2026-10-07。本文只记录本仓库已接线的能力和已观察到的结果；模型宣传能力、API 能力与工具能力分开判断。测试素材与 Key 均不提交到仓库。

## CogVideoX-3（`glm_video`）

依据：[智谱视频生成异步接口](https://docs.bigmodel.cn/api-reference/模型-api/视频生成异步)、[CogVideoX-3 使用说明](https://docs.bigmodel.cn/cn/guide/models/video-generation/cogvideox-3)。

| 官方字段 | 当前工具映射 | 核对结果 |
|---|---|---|
| `model` | 固定 `cogvideox-3` | 一致。 |
| `image_url` | 读取工作区 PNG/JPEG 原始字节，组为 `data:image/...;base64,...`；首尾帧为顺序数组 | 格式符合文档；工具不缩图、不重新编码。图片进入工作区之前若已缩小，工具不能恢复。 |
| `prompt` | `message`；仅当调用者传 `context` 时附加相关上下文 | 官方没有另一个图生视频专用提示词字段。图生视频应描述已有主体的动作，避免让模型重画人物。 |
| `quality` | 映射 `speed` / `quality`；现默认 `quality` | 文档明确 `speed` 较快但质量较低。手机历史调用显式用了 `speed`，因此修改默认值不会覆盖旧调用；工具说明已要求人像保真优先选 `quality`。 |
| `size` | 直接传官方尺寸枚举 | 手机历史调用使用 `720x1280`，属 720P 预览规格；本次对照使用 `1920x1080`。 |
| `fps` | 30 或 60，默认 30 | 一致。 |
| `with_audio` | 布尔值，默认 false | 官方字段，控制 AI 音效；不是工具自造参数。 |
| `duration` | 5 或 10 秒 | 一致。 |
| 多图 | 单首帧或严格首尾帧两张 | 官方未开放任意多图参考；discover 标为模型/API 限制。 |

实测输入 `baby-halfbody3.jpg` 是 720×504、58,092 字节。最近几次手机 GLM 调用明确传了 `quality:"speed"`、`size:"720x1280"`、30 fps。用同一张图片、同一动作提示词、`quality:"quality"`、1920×1080、30 fps、5 秒、`with_audio:true` 分别调用 MaiAgent 工具和官方接口直连，两条任务均成功；产物均为 1920×1080 H.264、约 5.13 秒且含 AAC。抽看的首帧和中段画面质量接近，未观察到工具下载链路额外降质。生成有随机性，不能要求两条视频像素一致。

## MiniMax H3（`minimax_video`）

依据：[V2 创建任务](https://platform.minimax.cn/docs/api-reference/video-generation-v2-create)、[V2 查询任务](https://platform.minimax.cn/docs/api-reference/video-generation-v2-query)、[文件上传](https://platform.minimax.cn/docs/api-reference/file-management-upload)、[错误码](https://platform.minimax.cn/docs/api-reference/errorcode)。视频工具已移除 Hailuo 2.3 V1 提交与查询路径；`minimax_image` 的 `image-01` 图片接口不受此变更影响。

| 官方字段/步骤 | 当前工具映射 | 边界 |
|---|---|---|
| `POST /v2/video_generation` | `delegate` 调 V2；默认 `MiniMax-H3`，可选 `MiniMax-H3-Max` | 付费调用仍需逐次确认。 |
| `content[]` 文本 | `message`、可选 `context` 和 `content` 中的文本合并为一项 `type:text` | 必须有非空文本；工具不把媒体 Base64 塞进文本字段。text 条目上误带的媒体 role 会被忽略。 |
| 首/尾帧 | 工作区路径先以 `purpose=video_generation_input` 上传，取得 `mm_file://`；按 `first_frame` / `last_frame` 填入 `image_url` | 支持 JPG/JPEG/PNG/WEBP/HEIC/HEIF；图生视频与多模态参考模式互斥。 |
| 多模态参考 | `content` 中按 `reference_image` / `reference_video` / `reference_audio` 逐项上传和引用；多图省略 role 或写 `reference` 会按媒体类型归一化 | 最多 9 图、3 视频、3 音频。单张无 role 图片按官方默认视为首帧；多张无 role 图片按参考图处理。文件规格由工具和平台共同校验。 |
| `duration` | H3：4–15 秒；H3 Max：5–15 秒 | 只接受顶层整数；缺失时明确指出顶层字段。 |
| `resolution` | H3：768P/2K；H3 Max：480P/768P | 只能放在顶层；H3 V2 **没有 1080P**，不会暗中升级收费或回退到 V1。 |
| `ratio` | 文生视频必须指定具体比例；首/尾帧输入传 `adaptive` | 首/尾帧场景的画幅由源图片决定；传 `9:16` 也不能强制把横图变竖图。 |
| 音频 | 可输入参考音频；输出音轨由 H3 生成 | 官方 V2 无独立 `with_audio` 开关；实测 H3 产物含 AAC。 |
| 异步终态 | `GET /v2/query/video_generation/{task_id}`，读取 `task.status` 与 `task.content.url` | `failed` 含平台错误码和原因；后台落库并通知主模型。暂时性查询错误有界重试。 |

`minimax_video action=validate` 复用正式提交的参数归一化与本地文件检查，返回带
`reference_image` 角色和文件大小的 `request_preview`，不读取密钥、不上传、不提交、
不计费。正式提交仍会逐个上传原文件，拿到 `mm_file://` 后构造 V2 `content[]`。
`duration` 与 `resolution` 必须是工具调用的顶层字段；缺少时会分别指出。

已完成的直接与原生工具对照：同一张 `seedream-baby-only.png`、同一提示词、4 秒/768P/源图自适应，官方直连与 MaiAgent 原生工具均出片；产物均为 1024×768 H.264、约 4.46 秒且含 AAC。首帧人物和服装保留，未观察到工具上传/下载额外压缩。

三图引用付费验收：三张同一人物的不同照片通过本地 `validate`，三项均归一化为
`reference_image`，随后由共享核心一次提交 H3 V2 成功（任务 ID
`449705623331104`）。服务商任务回执的 `usage.input_image_count` 为 **3**；
输出为 768×1344、约 4.46 秒的 H.264/AAC MP4。
这证明多图上传、角色映射、提交、查询、下载的链路可用；人物还原度仍由用户看成片
验收，iOS 宿主工具需随新包复测。

真人半身照 `baby-halfbody3.jpg` 的一条 10 秒/2K 直连任务被服务商拦截；相同文本不带图片的 4 秒/768P 任务成功。同一真人照通过 MaiAgent 原生工具生成的 4 秒/768P 和 **10 秒/2K** 两条任务均成功；后者产物为 2048×1440 H.264、10.13 秒、含 AAC。因此不能从一次笼统拦截推断“真人照一律不允许”，也不能判定是工具层系统性丢图。两次 10 秒/2K 请求的图片字节、提示词和主要生成参数一致，但上传记录不同；服务商的单次审核结果不稳定，具体触发项仍未由平台给出。用户界面应先呈现是否完成和下一步，而不是反复展示内部错误数字。

## 多图能力与主模型路由

Seedance 2.0 的[官方创建任务文档](https://docs.volcengine.com/docs/82379/1520757?lang=zh)使用 `content[]` 中重复的 `image_url` 项，并将每项 `role` 设为 `reference_image`，上限 9 张；不是单独的 `image_urls` 顶层数组。当时 `seedance_video` 按此格式接入 `reference_image_paths`，并在 discover 中标明上限和与严格首尾帧模式互斥。CogVideoX-3 仅支持单首帧或严格首尾帧两张。主模型提示词要求先查上限，不得静默丢图；超限时先拟定图像融合或可见排版方案，再走付费确认。

2026-10-08 更新：`seedance_video` 已改绑 Seedance 2.5。2.5 单次输出支持 4–30 秒，
参考图上限为 30 张，输出分辨率最高 1080p；首帧、编辑、延长任务的宽高比规则
与 2.0 不同，工具按任务类型提前校验并设置 `omni_reference_task_type`。
上述 2.0 实测结果保留为历史记录，不能作为 2.5 云端验收。
依据：[Seedance 2.5 教程](https://docs.volcengine.com/docs/ark/seedance-2-5)。

失败后的主模型回灌先区分平台明确给出的原因与推断。技术性图片问题按最小改动顺序处理并记录损失；每次付费重试都需要新确认。内容审核错误 `1026` 在 MiniMax 官方表中只定义为“输入内容涉敏”，不区分图片与文本，更没有单独的“真人儿童照片”代码。工具会保留原始错误与参考文件存在信息，不把不确定归因说成事实。
