# iOS 云盘和视频圈账号接口备用线路

## 背景与范围

- 2026-10-30，用户报告 Mac 登录二维码长期无法生成，并要求登录、API Key 和配置等账号域名请求在 DNS 出问题时使用备用线路。登录故障见 `maichat-macos-login-dns-20261030.md`；当时没有复现 iOS 云盘或视频圈的实际故障。
- 本次变更基于源码 `9348dccd0e720472fc3d05787f25945726caf7fe` 的工作树，给 iOS 云盘和视频圈的账号域名控制请求接入已探测的备用线路。文件列表、文件操作、上传签名、断点续传控制、VOD 签名刷新和视频圈接口共用账号选路。
- OSS 分片及签名上传、下载和其他供应商域名仍按各自原有网络策略处理；Session WebSocket 也不属于本次范围。

## 实现边界

- 跨平台 C++ 客户端在准确的 `https://ichat.life/` 控制请求前调用 iOS 提供的 selector；签名上传不使用账号线路。探测不携带业务凭据。DoH 备用线路直连，保留原 URL、SNI 和域名证书校验。
- 没有可用线路时不发送业务请求。传输错误只清除缓存，以便下次选路，不自动重发本次写操作。取消在选路前和发送前检查；正在进行的同步探测不能立即取消，最长等待约 9.5 秒。
- 平台未提供 selector 时维持既有代理行为；本次没有修改其他平台的接入配置。代码和后续修复提交号以版本控制记录为准。

## 验证记录

- 运行 `cmake --build /private/tmp/maichat-network-folders --target mai_cloud_drive_client_test mai_video_circle_client_test -j 4`，随后 `ctest --test-dir /private/tmp/maichat-network-folders -R 'mai_(cloud_drive|video_circle)_local_tls' --output-on-failure`：2/2 通过。测试覆盖无线路时拒绝账号写入、近似域名和第三方上传不调用账号 selector、本地 TLS 上传与取消、错误 CA 的拒绝。
- 指定 arm64 iOS 模拟器运行 `xcodebuild -workspace MaiChat.xcworkspace -scheme MaiChat -configuration Debug -destination 'platform=iOS Simulator,id=34893056-B994-4CFE-96C2-5526174AC370' -jobs 4 IPHONEOS_DEPLOYMENT_TARGET=16.0 build -quiet`：构建成功，日志 `/private/tmp/maichat-cloud-route-simulator-build2.log`。
- 本次尚未完成故障 DNS 下 iOS 云盘或视频圈真实服务的端到端验证，也未安装包含此变更的真机包；本地 TLS 测试不能证明生产备用线路已跑通。
