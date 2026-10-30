# Mac 创作中心模型连接超时（2026-10-30）

## 现场

- 用户报告 Mac 创作中心发送消息后显示 `curl: Timeout was reached`。只读查询当前账号的 Agent 数据库：最近一轮在北京时间 09:42:31 开始、09:44:02 结束，模型为 `deepseek-flash`，助手没有输出片段，也没有工具调用。此前数次失败轮次也没有助手输出。数据库没有保存这次 curl 的详细网络阶段。
- 现场进程 PID 13707 于 2026-10-28 15:19:31 启动，执行 `/Applications/MaiChat.app/Contents/MacOS/MaiChat`，版本 2.2.31、SHA-256 `9e66872cec131f075ae1c97c743be65e09952d1e708feb5ad2ad98d784635a8a`，与发布源码 `3446aefaa14315007b206f6f71030b0d732bfd74` 对应。进程没有重启或替换。
- 该错误由 Responses 模型客户端的 `curl_easy_perform` 返回，目标按该版本配置为 `https://api.deepseek.com/responses`。这是模型请求，与 IM 图片下载使用的路径不同。2026-10-29 的 `model stream inactive` 是另一种模型流无进展错误，不能用那次观察替代本次网络阶段证据。

## 网络证据与边界

- 10:50 左右的无凭据 HEAD 探测中，直连同域名在 DNS 阶段先后按 8 秒和 20 秒超时，没有建立 TCP 或 TLS；当前系统解析配置指向 utun6 的 DNS。显式使用本机系统 HTTP 代理访问同一地址约 0.29 秒收到 HTTP 401，证明当时代理链路完成了 TLS 和 HTTP，未发送模型任务。
- 系统 HTTP/HTTPS 代理已启用；现场进程环境中没有发现 curl 的代理环境变量，发布版模型客户端也没有给 curl 设置系统代理。Qt 5.15.18 的 `systemProxyForQuery` 对两个模型供应商地址返回 HTTP 代理。
- 以上是事后独立探测，不能证明 09:42 那次请求的解析或代理状态，也不能仅凭约 91 秒总时长确定当时每次尝试的阶段。发布版默认连接超时 20 秒，未输出时最多重试两次；它只显示 curl 的通用错误文字，没有保留该次更具体的错误缓冲区。

## 修改与验证

- 当前源码在 Mac 桌面的账号模型客户端按目标 URL 查询系统代理。Responses 客户端在宿主提供路由回调时逐跳选择代理，代理认证只用于对应跳，模型认证只发往原来源；HTTPS 不能重定向到明文连接，以免 307/308 将聊天正文发到明文地址。未提供回调的调用方保留原 curl 路径；iOS 不配置该回调。
- 本地可控假代理和模型接口验证不同跳使用不同代理、代理凭据隔离、302/307 方法、同源和跨源认证、直连、取消、单次总超时及未输出时重试；真实本地 TLS 服务的 302/307 降级测试确认明文目标没有收到请求。无凭据供应商 HEAD 探测不调用模型生成。
- 可重复执行：`cmake -S MaiAgent -B /tmp/maiagent-model-proxy-test-build -DMAIAGENT_BUILD_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Debug`；`cmake --build /tmp/maiagent-model-proxy-test-build --target MaiModelClientTests -j 4`；`/tmp/maiagent-model-proxy-test-build/bin/MaiModelClientTests`；`ctest --test-dir MaiChat/desktop/build-release-v2.2.31 -R '^agent_controller_test$' --output-on-failure`。
- Mac Release 候选位于 `MaiChat/desktop/build-release-v2.2.31/bin/MaiChat.app`，实际版本为 2.2.32，源码基线 `3f5cef49c` 加本次未提交改动，候选可执行文件 SHA-256 `8374d73e66071142fc2115e7c852fd4726eea900994fbe65faee883ab1f690f6`。构建目录名称不代表产物版本。修复提交尚未创建。
- 未在用户正在使用的 App 中部署或验证修复，未发起带凭据的真实模型请求，未覆盖全部真实 PAC、认证代理或 HTTPS 代理配置。候选与当前安装版之间还有其他源码变更，构建和定向测试不能证明用户现有功能均已在候选版验收。
