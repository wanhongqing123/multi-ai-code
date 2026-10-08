# MaiChat 素材签名服务

当前服务提供独立的火山 Ark Assets 素材接口，使用服务器持有的 AccessKey 签名。
已有方舟可访问的 HTTPS 图片 URL 时，可直接创建 AIGC 素材，不需要 OSS。
本地照片先上传到 OSS 的能力排在后续；**服务器签名接口已接入，不等于 App
本地照片一键入库已完成**。

原有 OSS 视频签名接口仍保留：它只签发临时链接，不接收视频字节。iPhone 将原始 MP4/MOV 直接 PUT 到私有
阿里云 OSS Bucket，再将最长 24 小时有效的签名 GET 链接交给 Seedance。视频不经过
腾讯 IM 云文件服务。方舟能否读取这个具体 Bucket 的签名链接，仍需配置完成后做一次
真实小视频验证。

## 阿里云侧准备

1. 创建**私有** OSS Bucket，记录 Bucket 名称和地域，如 `oss-cn-hangzhou`。
   给 `seedance-inputs/` 前缀设置约两天的生命周期删除规则，避免临时视频长期堆积。
2. 创建专用 RAM 身份，仅授予该 Bucket 的 `seedance-inputs/*` 对象
   `oss:PutObject`、`oss:GetObject` 权限。不要使用主账号 AccessKey。将下面的
   `YOUR_BUCKET` 换成实际 Bucket 名称后，可作为 RAM 自定义策略的起点：

   ```json
   {
     "Version": "1",
     "Statement": [{
       "Effect": "Allow",
       "Action": ["oss:PutObject", "oss:GetObject"],
       "Resource": ["acs:oss:*:*:YOUR_BUCKET/seedance-inputs/*"]
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
   | `MAICHAT_OSS_BUCKET`、`MAICHAT_OSS_REGION` | 可选，OSS Bucket 与地域 |
   | `ALIBABA_CLOUD_ACCESS_KEY_ID`、`ALIBABA_CLOUD_ACCESS_KEY_SECRET` | 可选，专用 OSS RAM 身份 |

   可以用 `openssl rand -hex 32` 生成令牌。长期 AK/SK 只放服务器配置，不填在
   MaiChat App，也不要提交到仓库。服务器只监听 `127.0.0.1:9000`，公网通过 HTTPS
   反向代理访问；所有请求都检查 Bearer 令牌。预留的火山 IAM API Key 另存于
   `/etc/maichat/volc-iam-api-key.json`（仅 root 可读），当前服务不会加载它。
   若使用函数计算，HTTP 触发器也必须保留鉴权；
   若改用现有业务后端，保持下面的接口格式即可。

## Ark Assets 素材接口

`POST /ark-assets` 的 JSON `action` 支持 `list_groups`、`create_group`、
`create_asset`、`get_asset`、`list_assets`。`create_asset` 接收 `group_id`、
方舟可访问的 HTTPS `url` 与可选名称；`get_asset` 返回实际异步状态，只有
`Active` 的素材 ID 才可作为 `asset://<ID>` 给 Seedance 使用。服务端不返回
AccessKey，且不接收图片字节。本地照片没有 HTTPS 地址时，需等待后续的 OSS 上传接线。

## App 配置

在 iOS MaiChat 的“模型配置”中填写素材服务 HTTPS 基址，例如
`https://ichat.life/maichat`，或旧式完整地址 `https://…/sign-upload`，再填写
同一个服务令牌。`ark_assets` 可经 `/ark-assets` 列出、创建和查询素材；
它不要求 OSS。`upload_image` 尚不能从 App 本地照片直接上传，会明确返回
`upload_not_configured`。`seedance_video discover` 只有在服务确认 OSS 已配置后，
才把 `video_edit_from_local_file` 上报为待实盘验证。Android、Desktop 的
素材服务仍需各自接线。

## 签名接口

请求：`POST /sign-upload`，Header 为 `Authorization: Bearer <令牌>`、
`Content-Type: application/json`，Body 示例：

```json
{"filename":"clip.mp4","size_bytes":20300000}
```

响应包含 `upload_url`、`upload_headers`（包括 Content-Type）和 `read_url`。
客户端按响应 Header 将**文件字节** PUT 至 `upload_url`；OSS 返回 2xx 后，
才把 `read_url` 交给方舟。PUT 链接有效 15 分钟，GET 链接有效 24 小时；
每次请求使用随机对象名，Bucket 保持私有。服务只允许 MP4/MOV 和声明不超过
200 MB 的文件，实际 Bucket 用量与访问权限仍应在阿里云控制台监控。

参考：[OSS 预签名 PUT 上传](https://help.aliyun.com/zh/oss/developer-reference/upload-objects-using-a-signed-url-generated-with-oss-sdk-for-node-js/)、
[V4 签名 GET/PUT](https://help.aliyun.com/zh/oss/developer-reference/add-signatures-to-urls)、
[OSS RAM 前缀权限](https://help.aliyun.com/zh/oss/user-guide/access-control-base-on-ram-policy)、
[函数计算 HTTP 触发器](https://help.aliyun.com/zh/functioncompute/http-trigger-invoking-function)。
