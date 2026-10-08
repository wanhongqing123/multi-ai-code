# MaiChat Seedance 私有媒体与 Ark Assets 服务

这个服务保管固定模型 API Key 和火山 AccessKey，向已鉴权 App 同步指定的模型
API Key，代签 Ark Assets 管理请求；阿里云 OSS 签发可最后配置。模型 API Key
同步后由 App 写入设备 Keychain；**完整 Key 会到达设备，不能把此方案当作密钥
完全不落地手机的安全边界**。若需要完全不向 App 下发长期 Key，模型请求须改为
服务器代发。现有 OSS 签发接口不接收媒体字节；iPhone 将
MP4/MOV 或 JPEG/PNG 直接 PUT 到私有阿里云 OSS Bucket，再将视频 24 小时、
图片 7 天有效的签名 GET 链接交给 Seedance 或 Ark Assets。媒体不经过
腾讯 IM 云文件服务。方舟能否读取这个具体 Bucket 的签名链接，仍需配置完成后做一次
真实小视频验证。

## 阿里云侧准备（仅本地文件中转需要）

1. 创建**私有** OSS Bucket，记录 Bucket 名称和地域，如 `oss-cn-hangzhou`。
   给 `seedance-inputs/` 和 `ark-asset-inputs/` 前缀设置生命周期删除规则，
   避免临时媒体长期堆积。素材异步入库，清理时间应覆盖方舟实际处理时间。
2. 创建专用 RAM 身份，仅授予该 Bucket 的两个临时前缀对象
   `oss:PutObject`、`oss:GetObject` 权限。不要使用主账号 AccessKey。将下面的
   `YOUR_BUCKET` 换成实际 Bucket 名称后，可作为 RAM 自定义策略的起点：

   ```json
   {
     "Version": "1",
     "Statement": [{
       "Effect": "Allow",
       "Action": ["oss:PutObject", "oss:GetObject"],
       "Resource": [
         "acs:oss:*:*:YOUR_BUCKET/seedance-inputs/*",
         "acs:oss:*:*:YOUR_BUCKET/ark-asset-inputs/*"
       ]
     }]
   }
   ```
## 服务端准备

使用 Node.js 20 或更高版本，执行 `npm ci --omit=dev` 安装依赖。可以部署到
函数计算（Handler `index.handler`），也可以在普通 Linux 云服务器上运行
`node server.js`。两种方式使用相同的环境变量：

   | 名称 | 内容 |
   |---|---|
   | `MAICHAT_OSS_BUCKET` | 可选，本地文件中转的 Bucket 名称 |
   | `MAICHAT_OSS_REGION` | 可选，本地文件中转的 OSS 地域 |
   | `ALIBABA_CLOUD_ACCESS_KEY_ID` | 可选，专用 RAM 身份的 AccessKey ID |
   | `ALIBABA_CLOUD_ACCESS_KEY_SECRET` | 可选，对应 Secret |
   | `MAICHAT_MEDIA_SERVICE_TOKEN` | 自行生成的至少 32 字符随机令牌；旧环境变量 `MAICHAT_OSS_SIGNER_TOKEN` 仍可用 |
   | `VOLC_ACCESS_KEY_ID` | 具备 Ark Assets 权限的火山引擎 AK，素材库接口必需 |
   | `VOLC_SECRET_ACCESS_KEY` | 对应 SK，素材库接口必需 |
   | `VOLC_ARK_PROJECT_NAME` | 可选，默认为 `default`，须与视频 API Key 所在项目相同 |
   | `MAICHAT_ARK_API_KEY` | 方舟 Seedance／Seedream 项目 API Key |
   | `MAICHAT_GLM_API_KEY` | GLM 主模型与图像／视频工具 API Key |
   | `MAICHAT_DEEPSEEK_API_KEY` | DeepSeek 主模型 API Key |
   | `MAICHAT_WAN_API_KEY`、`MAICHAT_WAN_WORKSPACE_ID` | 百炼 Wan／Qwen 凭据 |
   | `MAICHAT_KLING_API_KEY` | 可灵创作 API Key |
   | `MAICHAT_MINIMAX_API_KEY` | 海螺 MiniMax API Key |

可以用 `openssl rand -hex 32` 生成令牌。长期密钥只放服务器配置，不填在
MaiChat App，也不要提交到仓库。所有请求都由服务检查 Bearer 令牌。

服务端保管火山 Assets 管理 AK/SK 与可选的 OSS RAM AK/SK，不向 App 返回这两类
AccessKey。模型 API Key 通过下面的受控接口同步到 App Keychain，App 仍直接
调用模型。服务令牌本身也是敏感凭据，须放在设备 Keychain，不写进 URL、日志或
仓库。设备丢失时应撤销服务令牌并轮换已下发的模型 Key。

### 普通云服务器部署

`server.js` 默认只监听 `127.0.0.1:9000`，公网入口由 Caddy 提供 HTTPS：

```caddyfile
api.example.com {
    reverse_proxy 127.0.0.1:9000
}
```

将环境变量放到仓库外、仅管理员和服务账号可读的文件，例如
`/etc/maichat/media.env`。使用 `systemd` 的 `EnvironmentFile` 启动服务，
让它随系统启动并在异常退出后重启。只开放 80/443 给公网；SSH 仅对管理 IP
开放，9000 不对公网开放。域名和证书配置参考
[Caddy HTTPS 快速入门](https://caddyserver.com/docs/quick-starts/https)。

## App 配置

在 iOS／Android MaiChat 的“模型配置”中填写服务 HTTPS 地址，例如
`https://ichat.life/maichat`，以及服务令牌，点“保存配置”。旧的
`https://…/sign-upload` 配置仍兼容。保存时只把服务端已配置的模型 API Key
同步到 iOS Keychain／Android Keystore 加密文件，未配置的模型 Key 不会清除
本机原值。打开 AI 助手时也会尝试同步；网络不可用时继续使用本机已保存的 Key。
只配置火山 AK/SK 时，`ark_assets` 可使用已有 HTTPS 图片 URL 操作素材库；
`seedance_video discover` 的 `video_edit_from_local_file` 仍为
`upload_not_configured`。等 OSS 最后配置完成，该能力才进入待实盘验证状态。
Desktop 的云端密钥同步仍需接入，不能仅因移动端已接通就宣称可用。

已在 `ichat.life` 的 Nginx 部署时，`/maichat/` 路径转发到服务的本机
`127.0.0.1:9000`，只开放 POST，关闭访问日志并限流。可复用
[`deploy/nginx-location.conf.example`](deploy/nginx-location.conf.example)；
根站点和证书配置仍由现有 Nginx 管理。

## 模型 API Key 同步

`POST /credentials` 仅接受 Bearer 服务令牌。`{"action":"status"}` 返回已配置
模型名与火山 Assets／OSS 的配置状态，不返回密钥。`{"action":"fetch", "providers":
["ark","glm","deepseek","wan","kling","minimax"]}` 只返回服务端已配置且请求中列出的
模型 API Key；不支持读取火山或阿里云 AccessKey。所有响应带
`Cache-Control: no-store`，Nginx 入口应关闭该路径的访问日志并限制请求速率。

## 签名接口

请求：`POST /sign-upload`，Header 为 `Authorization: Bearer <令牌>`、
`Content-Type: application/json`，Body 示例：

```json
{"filename":"clip.mp4","size_bytes":20300000}
```

响应包含 `upload_url`、`upload_headers`（包括 Content-Type）和 `read_url`。
客户端按响应 Header 将**文件字节** PUT 至 `upload_url`；OSS 返回 2xx 后，
才把 `read_url` 交给方舟。PUT 链接有效 15 分钟，视频 GET 链接有效 24 小时，
图片 GET 链接有效 7 天，以覆盖方舟异步入库；
每次请求使用随机对象名，Bucket 保持私有。服务只允许 MP4/MOV 不超过 200 MB
或 JPEG/PNG 不超过 30 MB，实际 Bucket 用量与访问权限仍应在阿里云控制台监控。

## 私域虚拟人像素材库

同一函数提供 `POST /ark-assets`，使用相同的 Bearer 令牌。请求体的 `action`
分别为 `list_groups`、`create_group`、`create_asset`、`get_asset`、`list_assets`。
服务端使用火山 AK/SK 按官方签名方法调用 `ark.cn-beijing.volcengineapi.com`
的 `2024-01-01` 版 Ark Assets API。已有方舟可访问的图片 URL 时，直接调用
`ark_assets create_asset`，无需用户自备对象存储。本地图片没有现成 URL 时，
`ark_assets upload_image` 才可选先走 `/sign-upload` 暂存图片，再把临时 GET URL
提交给 Ark。Ark 完成入库后由方舟管理素材；阿里云 OSS 仅是可选的临时来源。
上传返回的素材仍可能是 `Processing`；必须通过 `get_asset` 查到
`Active`，才能把 `asset://<ID>` 给 `seedance_video.virtual_avatar_asset_id`。
`Failed` 时使用返回的 `Error.Code`/`Error.Message` 判断原因，不能当作成功。

Ark 的公开[私域虚拟人像库教程](https://docs.volcengine.com/docs/ark/private-virtual-avatar-library-guide-preview?lang=zh)
列明了 `CreateAssetGroup → CreateAsset → GetAsset` 的流程，也注明虚拟库的素材
要求及内容审核。具体照片能否入库以 Ark 返回的 `Active` 或 `Failed` 为准；
不能把素材提交成功等同于审核通过。真人人像 H5 授权链路是独立接口，当前服务未接入。

参考：[OSS 预签名 PUT 上传](https://help.aliyun.com/zh/oss/developer-reference/upload-objects-using-a-signed-url-generated-with-oss-sdk-for-node-js/)、
[V4 签名 GET/PUT](https://help.aliyun.com/zh/oss/developer-reference/add-signatures-to-urls)、
[OSS RAM 前缀权限](https://help.aliyun.com/zh/oss/user-guide/access-control-base-on-ram-policy)、
[函数计算 HTTP 触发器](https://help.aliyun.com/zh/functioncompute/http-trigger-invoking-function)。
