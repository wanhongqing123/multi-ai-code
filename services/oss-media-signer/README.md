# MaiChat 素材签名服务

当前服务提供独立的火山 Ark Assets 素材接口，使用服务器持有的 AccessKey 签名。
已有方舟可访问的 HTTPS 图片或视频 URL 时，可直接在指定素材组创建素材，不需要 OSS。
本地 JPEG/PNG 和 MP4/MOV 由 App 取得短期 PUT/GET 链接后直传私有 OSS，
再由服务器用 `CreateAsset` 登记。客户端等待 `GetAsset` 返回 `Active`，然后将
`asset://<ID>` 交给 Seedance；签名 GET 链接仅供方舟导入素材使用。文件字节不经过
签名服务或腾讯 IM 云文件服务。真实 Bucket 的导入过程仍需实盘验证。

## 阿里云侧准备

1. 创建**私有** OSS Bucket。当前目标是 `maichat-ai-video-resource`，地域
   `oss-cn-guangzhou`。给 `ark-asset-inputs/`、`seedream-inputs/`、
   `minimax-inputs/`、`kling-inputs/` 和 `wan-inputs/` 设置生命周期规则。
2. 创建专用 RAM 身份，仅授予该 Bucket 的 `ark-asset-inputs/*`、
   `seedream-inputs/*`、`minimax-inputs/*`、`kling-inputs/*` 与 `wan-inputs/*` 对象
   `oss:PutObject`、`oss:GetObject` 权限。不要使用主账号 AccessKey。将下面的
   `YOUR_BUCKET` 换成实际 Bucket 名称后，可作为 RAM 自定义策略的起点：

   ```json
   {
     "Version": "1",
     "Statement": [{
       "Effect": "Allow",
       "Action": ["oss:PutObject", "oss:GetObject"],
       "Resource": ["acs:oss:*:*:YOUR_BUCKET/ark-asset-inputs/*",
                    "acs:oss:*:*:YOUR_BUCKET/seedream-inputs/*",
                    "acs:oss:*:*:YOUR_BUCKET/minimax-inputs/*",
                    "acs:oss:*:*:YOUR_BUCKET/kling-inputs/*",
                    "acs:oss:*:*:YOUR_BUCKET/wan-inputs/*"]
     }]
   }
   ```
3. 可以在函数计算运行 `index.handler`，也可在普通 Linux 云服务器上运行
   `node server.js`，由 Nginx 为其提供 HTTPS 入口。运行环境使用 Node.js 20 及以上；
   OSS 功能需要执行 `npm ci --omit=dev` 安装依赖，只有 Ark Assets 时不需要 OSS SDK。
4. 在**服务器受限环境文件**配置：

   | 名称 | 内容 |
   |---|---|
   | `VOLC_ACCESS_KEY_ID`、`VOLC_SECRET_ACCESS_KEY` | 火山 Assets API 签名凭据 |
   | `VOLC_ARK_PROJECT_NAME` | 方舟项目名，默认 `default`，须与生成任务使用的项目一致 |
   | `MAICHAT_AUTH_PORT` | 本机账号服务端口，默认 9100；素材操作会用账号 Bearer 向它核验权限 |
   | `MAICHAT_OSS_BUCKET`、`MAICHAT_OSS_REGION` | 可选，OSS Bucket 与地域 |
   | `ALIBABA_CLOUD_ACCESS_KEY_ID`、`ALIBABA_CLOUD_ACCESS_KEY_SECRET` | 可选，专用 OSS RAM 身份 |

   可以用 `openssl rand -hex 32` 生成令牌。长期 AK/SK 只放服务器配置，不填在
   MaiChat App，也不要提交到仓库。服务器只监听 `127.0.0.1:9000`，公网通过 HTTPS
   反向代理访问；素材请求只接受已登录账号的 Bearer 令牌。预留的火山 IAM API Key 另存于
   `/etc/maichat/volc-iam-api-key.json`（仅 root 可读），当前服务不会加载它。
   若使用函数计算，HTTP 触发器也必须保留鉴权；
   若改用现有业务后端，保持下面的接口格式即可。

## 服务职责

`POST /sign-upload` 和 `POST /ark-assets` 接收当前登录账号的 Bearer 令牌，
本进程调用本机账号服务 `GET /v1/media/authorize` 校验权限。
模型 API Key 与临时凭据由独立的 `services/model-credentials` 进程签发，
本进程不保存供应商模型 Key，也不下发任何共享服务令牌。

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
AccessKey，且不接收文件字节。已登录账号的 App 可通过共享 C++ 上传工具先把
工作区 JPEG/PNG 或 MP4/MOV 直传 OSS，再将限时读取 URL 交给本接口。
素材类型参见[方舟私有形象库官方指引](https://docs.volcengine.com/docs/ark/private-virtual-avatar-library-guide-preview?lang=zh)。

## App 使用方式

App 登录后使用当前账号会话访问素材接口，不需要输入服务地址或共享令牌。
`ark_assets` 可经 `/ark-assets` 列出、创建和查询素材；
已有 HTTPS URL 不要求 OSS。`upload_image` 从 App 工作区读取 JPEG/PNG，向签发服务
取短期 URL，再用共享 C++ 直传 OSS。`upload_video` 对 MP4/MOV 采用同一路径，
待 Ark 素材进入 `Active` 后才能供 Seedance 使用。三端的 `ark_assets` 共用
MaiAgent C++ 上传与服务端接口；模型 Key 则由账号服务分别下发。

## 签名接口

请求：`POST /sign-upload`，Header 为 `Authorization: Bearer <账号访问令牌>`、
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
`ark-asset-inputs/`；`seedream_input` 只允许图片，写入 `seedream-inputs/`；
`minimax_input` 允许图片和视频，写入 `minimax-inputs/`；`kling_input` 允许图片和
视频，写入 `kling-inputs/`；`im_avatar` 仅允许不超过 2 MB 的 JPEG/PNG，写入
`avatars/`。头像不设置到期自动删除规则。
缺少用途、未知用途或用途与文件类型不符时
返回 400。对象路径由服务器生成，
请求中的路径或前缀字段不能覆盖它。上线新客户端前，先为上述五个目录部署 RAM
权限与生命周期规则，并让全部调用方带上用途字段。
实际 Bucket 用量与访问权限仍应在阿里云控制台监控。

## IM 头像

已登录账号使用 `purpose=im_avatar` 请求 `/maichat/sign-upload`，将图片直传私有
OSS。上传成功后，将响应里的 `avatar_url` 写入腾讯 IM 自身资料的 `faceURL`。
`avatar_url` 是固定的 `https://ichat.life/maichat/avatars/...` 地址，不包含
临时 OSS 签名；各端读取头像时，该地址返回 302 到 5 分钟有效的 OSS GET 链接，
图片字节不经过 MaiChat 服务。头像对象名使用随机 UUID，新头像产生新 URL，旧头像
继续可读，避免影响已同步的 IM 资料与本地缓存。

参考：[OSS 预签名 PUT 上传](https://help.aliyun.com/zh/oss/developer-reference/upload-objects-using-a-signed-url-generated-with-oss-sdk-for-node-js/)、
[V4 签名 GET/PUT](https://help.aliyun.com/zh/oss/developer-reference/add-signatures-to-urls)、
[OSS RAM 前缀权限](https://help.aliyun.com/zh/oss/user-guide/access-control-base-on-ram-policy)、
[函数计算 HTTP 触发器](https://help.aliyun.com/zh/functioncompute/http-trigger-invoking-function)。
