# MaiChat 素材签名服务

当前服务提供独立的火山 Ark Assets 素材接口，使用服务器持有的 AccessKey 签名。
已有方舟可访问的 HTTPS 图片或视频 URL 时，可直接在指定素材组创建素材，不需要 OSS。
本地 JPEG/PNG 和 MP4/MOV 由 App 取得短期 PUT/GET 链接后直传私有 OSS，
再由服务器用 `CreateAsset` 登记。客户端等待 `GetAsset` 返回 `Active`，然后将
`asset://<ID>` 交给 Seedance；签名 GET 链接仅供方舟导入素材使用。文件字节不经过
签名服务或腾讯 IM 云文件服务。真实 Bucket 的导入过程仍需实盘验证。

## 阿里云侧准备

1. 创建**私有** OSS Bucket。当前目标是 `maichat-ai-video-resource`，地域
   `oss-cn-guangzhou`。给 `ark-asset-inputs/`、`wan-inputs/` 和
   `shared-media-inputs/` 设置生命周期规则。
2. 创建专用 RAM 身份，仅授予该 Bucket 的 `ark-asset-inputs/*`、
   `wan-inputs/*` 与 `shared-media-inputs/*` 对象
   `oss:PutObject`、`oss:GetObject` 权限。不要使用主账号 AccessKey。将下面的
   `YOUR_BUCKET` 换成实际 Bucket 名称后，可作为 RAM 自定义策略的起点：

   ```json
   {
     "Version": "1",
     "Statement": [{
       "Effect": "Allow",
       "Action": ["oss:PutObject", "oss:GetObject"],
       "Resource": ["acs:oss:*:*:YOUR_BUCKET/ark-asset-inputs/*",
                    "acs:oss:*:*:YOUR_BUCKET/wan-inputs/*",
                    "acs:oss:*:*:YOUR_BUCKET/shared-media-inputs/*"]
     }]
   }
   ```
3. 可以在函数计算运行 `index.handler`，也可在普通 Linux 云服务器上运行
   `node server.js`，由 Nginx 为其提供 HTTPS 入口。运行环境使用 Node.js 20 及以上；
   OSS 功能需要执行 `npm ci --omit=dev` 安装依赖，只有 Ark Assets 时不需要 OSS SDK。
4. 在**服务器受限环境文件**配置：

   | 名称 | 内容 |
   |---|---|
   | `MAICHAT_MEDIA_SERVICE_TOKEN` | 至少 32 字符的客户端 Bearer 令牌；兼容旧名 `MAICHAT_OSS_SIGNER_TOKEN` |
   | `VOLC_ACCESS_KEY_ID`、`VOLC_SECRET_ACCESS_KEY` | 火山 Assets API 签名凭据 |
   | `VOLC_ARK_PROJECT_NAME` | 方舟项目名，默认 `default`，须与生成任务使用的项目一致 |
   | `MAICHAT_AUTH_SERVICE_TOKEN` | 与账号服务一致、仅供两进程回环调用的独立令牌，不写入 App |
   | `MAICHAT_ARK_MODEL_IDS` | 允许签发临时 Key 的 Seedance 与 Seedream Model ID 白名单，逗号分隔 |
   | `MAICHAT_GLM_API_KEY`、`MAICHAT_DEEPSEEK_API_KEY` | 可选，GLM、DeepSeek 模型 API Key |
   | `MAICHAT_WAN_API_KEY`、`MAICHAT_WAN_WORKSPACE_ID` | 可选，百炼 API Key 和 Workspace ID |
   | `MAICHAT_MINIMAX_API_KEY` | 可选，海螺 API Key；百炼可灵复用万相 Key |
   | `MAICHAT_OSS_BUCKET`、`MAICHAT_OSS_REGION` | 可选，OSS Bucket 与地域 |
   | `ALIBABA_CLOUD_ACCESS_KEY_ID`、`ALIBABA_CLOUD_ACCESS_KEY_SECRET` | 可选，专用 OSS RAM 身份 |

   可以用 `openssl rand -hex 32` 生成令牌。长期 AK/SK 只放服务器配置，不填在
   MaiChat App，也不要提交到仓库。服务器只监听 `127.0.0.1:9000`，公网通过 HTTPS
   反向代理访问；所有请求都检查 Bearer 令牌。预留的火山 IAM API Key 另存于
   `/etc/maichat/volc-iam-api-key.json`（仅 root 可读），当前服务不会加载它。
   若使用函数计算，HTTP 触发器也必须保留鉴权；
   若改用现有业务后端，保持下面的接口格式即可。

## 模型 API Key 同步

`POST /credentials` 只接受服务 Bearer 令牌。`{"action":"status"}` 返回已配置的
模型名和 Assets／OSS 状态，不返回密钥。`{"action":"fetch","providers":
["ark","glm","glm_video","deepseek","wan","kling","minimax"]}` 只返回请求中指定且
服务端已配置的模型 API Key；`ark` 始终为空，不再下发长期方舟 Key。
`glm` 给 GLM 主模型，`glm_video` 是旧客户端兼容字段；
两个下发项都读取同一个 `MAICHAT_GLM_API_KEY`，不需要第二把 GLM Key。
火山和阿里云 AccessKey、预留的 IAM API Key 不在返回范围。
响应禁止缓存，Nginx 入口关闭访问日志并限制速率。

iOS、Android、Desktop 取回模型 Key 后仅在当前 App 进程内使用，不再保存本机副本；
旧的本机模型 Key 会在升级后清理。App 重启后需要重新连接此服务，离线时不能用
云端模型。连接服务所需的地址和令牌仍由 App 保存，未来账号登录后再替换这层认证。
GLM、DeepSeek、MiniMax 等旧同步入口仍可能把长期 Key 发到设备，不能视作
账号鉴权完成；正式开放多账号前需分别迁移成短期凭据或账号隔离的 Key。

## 方舟媒体模型短期 Key

`POST /temporary-credentials` 只接受账号服务持有的独立服务令牌，本机回环调用；
Nginx 明确拒绝公网同路径。素材服务以火山 AK/SK 调用 `GetApiKey`，只允许环境变量
`MAICHAT_ARK_MODEL_IDS` 中的 Seedance / Seedream 预置模型，固定有效期 300 秒。账号服务再用手机号
登录会话和单独的模型白名单审核后发给 App。App 直连方舟生成图片／视频、查询或取消任务，
AK/SK 和长期 Ark Key 都不下发。Seedream 与 Seedance 使用同一枚短期 Key。
临时 Key 一旦泄漏，在到期前仍可使用；过期后
App 必须重新领取。旧版 App 的共享服务令牌在生产切换时必须轮换。

## 百炼短期 Key

同一内网 `POST /temporary-credentials` 接受 `action=bailian_video`，使用服务器上的
`MAICHAT_WAN_API_KEY` 调用北京地域百炼 `POST /api/v1/tokens`，固定有效期 300 秒。
账号服务按登录会话和模型白名单下发临时 `st-` Key 与 Workspace ID；App 仅在内存
持有，万相和百炼可灵共用。旧 `/credentials` 不再下发万相或可灵长期 Key。
临时 Key 继承主 Key 的全部权限，且到期前无法主动撤销；应把主 Key 限制到专用
业务空间和模型，控制费用配额。服务端上线与移动端升级需协调，旧包在切换后将
无法通过旧接口获取万相或可灵 Key。

## Ark Assets 素材接口

`POST /ark-assets` 的 JSON `action` 支持 `list_groups`、`create_group`、
`get_group`、`create_asset`、`get_asset`、`list_assets`、`begin_real_validation`、
`get_real_validation`。`list_groups` / `list_assets` 可带 `group_type=LivenessFace`
查询本账号真人组，默认 `AIGC`。`begin_real_validation` 用服务器固定的 CallbackURL
向方舟创建一次性 H5Link；本人完成认证后，用返回的 BytedToken 调
`get_real_validation` 取得 GroupId。H5 回调页为
`GET /maichat/portrait-auth-callback`，不展示或记录 BytedToken。
`get_group` 接收 `group_id` 并用方舟 `GetAssetGroup` 查询组信息，可核对返回的
`GroupType` 是 `AIGC` 还是 `LivenessFace`。
`create_asset` 接收 `group_id`、方舟可访问的 HTTPS `url`、可选名称和可选的
`asset_type`。`asset_type` 只接受 `Image` 或 `Video`，省略时使用 `Image`；
服务端分别传给方舟 `CreateAsset` 的 `AssetType` 字段。
可选 `expected_group_type` 只接受 `AIGC` 或 `LivenessFace`。提供此字段时，
服务端先调用 `GetAssetGroup` 核对实际组类型，不符或查询失败就不创建素材；
真人视频应传 `expected_group_type=LivenessFace`，以免写入 AIGC 组。
`get_asset` 返回实际异步状态，只有
`Active` 的素材 ID 才可作为 `asset://<ID>` 给 Seedance 使用。服务端不返回
AccessKey，且不接收文件字节。已配置签发服务的 App 可通过共享 C++ 上传工具先把
工作区 JPEG/PNG 或 MP4/MOV 直传 OSS，再将限时读取 URL 交给本接口。
素材类型参见[方舟私有形象库官方指引](https://docs.volcengine.com/docs/ark/private-virtual-avatar-library-guide-preview?lang=zh)。

## App 配置

在 iOS MaiChat 的“模型配置”中填写素材服务 HTTPS 基址，例如
`https://ichat.life/maichat`，或旧式完整地址 `https://…/sign-upload`，再填写
同一个服务令牌。`ark_assets` 可经 `/ark-assets` 列出、创建和查询素材；
已有 HTTPS URL 不要求 OSS。`upload_image` 从 App 工作区读取 JPEG/PNG，向签发服务
取短期 URL，再用共享 C++ 直传 OSS。`upload_video` 对 MP4/MOV 采用同一路径，
待 Ark 素材进入 `Active` 后才能供 Seedance 使用。三端的 `ark_assets` 共用
MaiAgent C++ 上传与服务端接口；iOS／Android 打开 AI 助手时会获取当前模型 Key，
Desktop 在连接服务时获取，均不依赖旧的本机 Key 缓存。

## 签名接口

请求：`POST /sign-upload`，Header 为 `Authorization: Bearer <令牌>`、
`Content-Type: application/json`，Body 示例：

```json
{"filename":"clip.mp4","size_bytes":20300000,"purpose":"ark_asset_input"}
```

响应包含 `upload_url`、`upload_headers`（包括 Content-Type）和 `read_url`。
客户端按响应 Header 将**文件字节** PUT 至 `upload_url`；OSS 返回 2xx 后，
才把 `read_url` 交给方舟。PUT 链接有效 15 分钟，GET 链接有效 24 小时；
每次请求使用随机对象名，Bucket 保持私有。服务允许 JPEG/PNG（不超过 30 MB）、
MP4/MOV（不超过 200 MB），以及 Wan3 参考文档 DOC/DOCX、XLS/XLSX、
PPT/PPTX、PDF、TXT、KEY、PAGES、NUMBERS、MD（不超过 100 MB）。
`purpose` 为必填，只能取服务端固定值：`wan_input` 允许上述图片、视频和文档，写入
`wan-inputs/`；`ark_asset_input` 只允许 JPEG/PNG、MP4/MOV，写入
`ark-asset-inputs/`；
`shared_media_input` 供非 Wan、非 Seedance 的通用素材调用，统一写入
`shared-media-inputs/`。
缺少用途、未知用途或用途与文件类型不符时
返回 400。对象路径由服务器生成，
请求中的路径或前缀字段不能覆盖它。上线新客户端前，先为 `wan-inputs/*` 和
`shared-media-inputs/*` 部署 RAM 权限与生命周期规则，并让全部调用方带上用途字段。
实际 Bucket 用量与访问权限仍应在阿里云控制台监控。

参考：[OSS 预签名 PUT 上传](https://help.aliyun.com/zh/oss/developer-reference/upload-objects-using-a-signed-url-generated-with-oss-sdk-for-node-js/)、
[V4 签名 GET/PUT](https://help.aliyun.com/zh/oss/developer-reference/add-signatures-to-urls)、
[OSS RAM 前缀权限](https://help.aliyun.com/zh/oss/user-guide/access-control-base-on-ram-policy)、
[函数计算 HTTP 触发器](https://help.aliyun.com/zh/functioncompute/http-trigger-invoking-function)。
