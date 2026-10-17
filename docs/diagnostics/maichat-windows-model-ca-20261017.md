# MaiChat Windows 创作中心模型证书验证失败

- 时间与现场：2026-10-17，Windows x64。用户在已安装的 MaiChat Desktop 2.1.2 创作中心发送“你好”，界面显示 `curl: SSL certificate problem: unable to get local issuer certificate`。运行进程为 `D:/Program Files/MaiChat/maichat.exe`；同日应用启动日志确认版本 2.1.2 和该路径。错误发生在 HTTPS 证书验证阶段，尚未收到模型响应。
- 源码基线：`ac7aa0de1224c559407b62d023f1801e2df87b21`。`MaiResponsesClient` 和 `MaiOpenAiClient` 在 Windows 使用内嵌 OpenSSL libcurl，但没有 CA 路径时未设置 `CURLSSLOPT_NATIVE_CA`。同一产品的 `DesktopCurlAuthTransport` 已显式使用 Windows 受信任根，因此扫码登录与模型请求结果不同。内嵌 curl 不会自动继承 Windows 根证书库；受信任根来源缺失是本次证书错误的直接原因。
- 红绿复现：`MaiModelClientTests` 的 `MAI_MODEL_LIVE_TLS_TEST=1` 使用空 API Key，向 GLM Responses 地址发一条无付费探针。修复前在本机得到与截图一致的 `curl: SSL certificate problem: unable to get local issuer certificate`，测试失败。修复后同一探针通过 TLS，结果变为 `Responses stream ended without a terminal event`、`MaiErrorCode::Protocol`；它只证明证书检查已通过，不证明带真实 Key 的模型生成成功。
- 修复提交：`7fff1769848480c0fb9edbdc90a91699e1e5bde9`。两个模型客户端仅在 `_WIN32` 且没有显式 CA 文件时使用 `CURLSSLOPT_NATIVE_CA`；显式 CA 路径保持原行为，证书及主机名校验保持开启。Mac/iOS/Android 不进入新增分支。
- 验证：Windows MSVC 2022 Release `MaiModelClientTests` 无 Key 现场探针由红变绿；MaiAgent 相关 9 组 CTest 9/9 通过。Windows MSVC 2019 Release `maichat` 重新链接，桌面相关 4 组 CTest 4/4 通过；`npm run check:line-width`、`git diff --check` 通过。原机正在运行的安装版没有被停止或替换。
- 边界：未使用账号 Key 发起付费模型对话，也未将新程序安装到用户当前运行的目录；用户截图仍来自原 2.1.2 安装包。Mac/iOS/Android 构建需在对应平台再确认，不能仅凭 Windows 预处理条件宣称全平台实测通过。
