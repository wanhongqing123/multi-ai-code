# MaiChat Seedance 视频 OSS 签名服务

这个函数只签发临时链接，不接收视频字节。iPhone 将原始 MP4/MOV 直接 PUT 到私有
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
3. 在函数计算创建 Node.js 20 函数，运行 Handler `index.handler`，配置 HTTPS
   HTTP 触发器。函数代码和依赖可在本目录执行 `npm ci --omit=dev` 后打包上传。
4. 在**函数环境变量**配置：

   | 名称 | 内容 |
   |---|---|
   | `MAICHAT_OSS_BUCKET` | Bucket 名称 |
   | `MAICHAT_OSS_REGION` | `oss-cn-hangzhou` 这类 OSS 地域 |
   | `ALIBABA_CLOUD_ACCESS_KEY_ID` | 专用 RAM 身份的 AccessKey ID |
   | `ALIBABA_CLOUD_ACCESS_KEY_SECRET` | 专用 RAM 身份的 Secret |
   | `MAICHAT_OSS_SIGNER_TOKEN` | 自行生成的至少 32 字符随机令牌 |

   可以用 `openssl rand -hex 32` 生成令牌。长期 OSS 密钥只放函数配置，不填在
   MaiChat App，也不要提交到仓库。函数的 HTTP 触发器由代码检查 Bearer 令牌；
   若改用现有业务后端，保持下面的接口格式即可。

## App 配置

在 iOS MaiChat 的“模型配置”中填写函数完整的 HTTPS 地址
`https://…/sign-upload` 与同一个签名服务令牌，点“保存配置”。
`seedance_video discover` 的 `video_edit_from_local_file` 会从
`upload_not_configured` 变为 `implemented_unverified`。目前 Android、Desktop
仍需各自接入签名服务配置，不能仅因已创建 Bucket 就上报可用。

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
