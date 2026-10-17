# MaiChat 模型凭据服务

本进程只保管并签发模型凭据，不处理账号登录、OSS 文件或素材登记。
它仅监听本机 `127.0.0.1:9200`，不配置公网 Nginx 路由。账号服务验证
会话与模型权限后，携带只在两个服务进程中保存的令牌调用：

- `POST /account-model-keys`，`{"action":"account_models"}`：返回 GLM、DeepSeek、MiniMax Key。
- `POST /temporary-credentials`，`{"action":"ark_seedance"}`：返回五分钟的 Seedance/Seedream Key。
- `POST /temporary-credentials`，`{"action":"bailian_video"}`：返回五分钟的百炼 Key 与 Workspace ID。

App 只调用账号服务的公开接口；取得凭据后直接请求模型供应商。
本进程不转发模型请求、流式响应或素材字节。环境变量见
`deploy/model-credentials.env.example`；真实密钥只保存在服务器受限环境文件中。
