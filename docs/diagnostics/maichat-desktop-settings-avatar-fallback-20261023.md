# Desktop 设置页头像回退为默认图

- 现场：2026-10-23，用户截图中设置页顶部与左侧账号头像均显示默认插画；账号昵称和 MaiChat ID 正常。现场进程为 `/Applications/MaiChat.app` 2.2.11，未停止或替换。
- 证据：当前进程日志多次记录 `self profile updated; has avatar: true`。腾讯 IM 本地资料库确有一条 `ichat.life` 的 HTTPS 头像 URL；独立只读 GET 返回 200、`image/jpeg`。设置页 `selfProfileAvatar` 未设置 `avatarRequestsEnabled`，而头像下载函数在该属性为 false 时直接返回。`selfProfileChanged` 回调只更新左侧 `navLogo`，没有刷新设置页；下载失败 URL 又会一直停留在失败集合，直到 IM 连接状态变化。
- 修复：设置页头像控件启用图片请求；收到本人资料变化时同时刷新设置页。失败下载增加 30 秒后重试，日志仅记录域名、HTTP 状态和错误码，不记录完整头像 URL。
- 验证：`MainWindowLayoutTest::settingsAvatarUpdatesWhenSelfProfileArrives` 使用本地 HTTP 图片服务，确认资料到达后设置页实际绘制真实图片；其头像请求属性断言针对旧代码缺失的设置，修后通过。当前源码构建的 Mac App 已重链；用户正在运行的 2.2.11 未覆盖，真实线上头像需在新包安装后复核。

## 2026-10-24 再次回退（Mac 2.2.18）

- 复现：用户打开设置页，左侧导航与账号卡同时显示默认插画，昵称与账号 ID 正常。运行进程是独立包 `dist-session-lifecycle-v2218/MaiChat.app` 2.2.18，二进制 SHA-256 为 `aecb0c248981ce930f7e7dce3fbedbbae6e5a5697523dd5acdf1f038560853ab`；本轮源码基线提交 `8ac971d3104ae447f965e8fb6ea0c0d479e4f248`，头像修复仍是工作树改动。
- 直接证据：进程日志 `self profile updated; has avatar: true`，随后对头像域名反复记录 `status=0 error=2`，即 Qt 网络组件未取得 HTTP 响应。前一轮的 UI 请求开关与资料刷新都已生效；这次缺的是图片字节。macOS 系统代理为本机 HTTP 代理；隔离探针中，Qt 默认网络管理器及显式系统代理均报错，而项目已经链接的 OpenSSL 版 libcurl 通过同一系统代理返回 200。只读检查 SDK 本地资料中的当前头像 URL，并用 OpenSSL 代理路径取回 87,145 字节图片；服务端本机 HTTPS 也返回 200。未在日志或记录中输出完整头像 URL。
- 根因：Mac 头像仍走 Qt `QNetworkAccessManager` 下载；当前机器的系统代理与 DNS 环境下，这条链路对 `ichat.life` 失败。失败重试会再次请求同一不可达链路，进程内图片缓存也在 App 重启时消失，因而用户反复看到默认插画。
- 修复：桌面头像统一走项目已有的 libcurl 异步下载链路；各平台只分别提供系统代理、Mac 内嵌 CA 或 Windows 原生 CA。HTTPS 下载与重定向只允许 HTTPS，HTTP 只允许 HTTP/HTTPS，保持证书校验与 4 MiB 限制。成功解码的 320px 内头像以完整 URL 的 SHA-256 命名，原子写入应用缓存；仅在当前资料提供完全相同的 URL 时恢复，并校验文件类型、尺寸与大小。
- 验证：`settingsAvatarUpdatesWhenSelfProfileArrives`、`settingsAvatarSurvivesNetworkLossAfterRestart` 均通过；后者在本地 HTTP 图片服务关闭后启动新的测试进程，仍绘制已缓存头像。删除测试进程先前头像缓存后，`settingsAvatarLoadsRealProfileUrl` 用只读 SDK 资料中的当前头像 URL 通过新链路重新取图，两个控件均从默认图变成真实照片；亲看 QWidget 自身绘制的 `/tmp/maichat-avatar-live-shared-curl.png`，照片与用户原来的头像一致。设置页与联系人页旧功能定向用例也通过。独立打包 `dist-avatar-v2219/MaiChat.app` 2.2.19，二进制 SHA-256 `f97e09173419e74bfe1da5a19443db941b5c33844163be7c39f690c814add68e`；严格签名验证及打包版 `--smoke` 均通过。当前正在运行的 2.2.18 仍是旧二进制，尚未切换，不宣称其界面已修复。

## 运行中的 2.2.19 复核

- 2026-10-24 00:45 从独立 `dist-avatar-v2219/MaiChat.app` 启动，进程路径和日志版本均为 2.2.19。新进程先停在登录页；Desktop 按现有策略不跨进程保存刷新令牌，非本次头像修复引入。
- 00:57 用户完成登录后，运行日志出现 IM 登录成功与 `self profile updated; has avatar: true`。产品缓存 `~/Library/Caches/MaiChat/Desktop IM/avatars/` 生成 URL 哈希命名的 PNG。只读打开该缓存文件，图像是用户此前使用的黄色沙发照片，证明运行中的 2.2.19 已从当前头像 URL 获取并持久化原图。没有使用 macOS 整屏录制；当前窗口的像素画面未直接抓取，设置页和导航头像同源绘制路径由上述 Qt 测试覆盖。
