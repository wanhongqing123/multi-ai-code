# 桌面端模型临时凭据接入

账号服务已提供 `GET /v1/model/seedance-credential` 与
`GET /v1/model/bailian-credential`。两者返回短期 `api_key` 和 `expires_at`；
百炼还返回 `wan_workspace_id`。因此 Seedance、Seedream 所用的 Ark 密钥，以及
Wan、Kling 所用的百炼密钥，可以直接用桌面账号会话领取，无需等待其它模型接口。

桌面端已在 `DesktopAuthClient` 用内存中的访问令牌领取 Ark/百炼临时凭据，
每 30 秒检查到期时间，在到期前 90 秒续领。`AgentController` 的对应媒体工具
通过线程安全的实时提供者，在每次调用前读取仍有效的密钥，而不是在构造时
复制一次。领取接口遇到 401 会单飞刷新访问令牌并重试一次；403 停止本会话
继续探测该模型权限。退出或会话失效时立即清空缓存。Ark/百炼密钥和工作空间
ID 不写入 QSettings、文件或日志。

GLM、DeepSeek、MiniMax 目前仍由桌面端与 iOS 的旧共享服务提供凭据。
账号服务没有对应的短期签发接口；它们的迁移需要后续新增提供者能力或代理，
与现有 Ark/百炼端点接入互不依赖。桌面端的模型选择、会话和非敏感服务地址
可以继续保留在设置中。旧共享服务的访问令牌已移入独立的系统安全存储项；
旧版 QSettings 明文在首次读取时迁移；迁移失败也立即删除明文。若没有有效的
`MAICHAT_MEDIA_SERVICE_TOKEN` 环境令牌，设置页会提示重新输入；若已有有效
环境令牌，则继续使用它，不在 INI 中等待下次重试。
