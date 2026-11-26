# Multi-AI Code 2.2.7

Windows x64 / macOS arm64 联合发布，内置 Codex `0.172.0+multi-ai.3d86e43b07`。

## 更新

- Codex 核心同步到官方 `rust-v0.172.0`，保留结构化 IM、逐条回复、审批、侧会话自动返回及宿主主题控制。
- 普通交互、resume/fork/agents 显式使用内嵌 app-server；保留主动指定的远端及服务命令，普通宿主会话不依赖后台 daemon。
- Code Mode 使用上游 gRPC 通道，实包验证双会话隔离、本地 V8 计算和正常关闭。
- 适配服务版本可缺省的新接口，保留 Windows 原生路径确认框；修正大型事件 future 的堆分配边界，保持 8 MiB 栈预算与事件顺序。

## 构建与实际验收

- 两端产品源码固定为 `39e8eed575d9978535eba59a7a72a3a8a9ad1206`；Codex 子仓为 `6e09a1b58f759848942e3d5b7653ab714f045ada`，官方基线为 `3d86e43b075adeba2193ec5c22ac92f55afd82e6`。
- 使用 Rust 1.99.1 / release thin LTO / 4 codegen units。Windows 为本机 2 jobs、源码 line-tables-only；Mac 为本机独立快照与私有工具链、3 jobs、debug=0、关闭增量编译。Mac 正式产物来自本机构建，不是失败的 Actions 产物。
- Windows：实际 staging 与 ZIP 副本，版本/manifest、PE 依赖、Electron 原生模块、官方 Rust 与独立 Node gRPC 客户端、GUI 和宿主默认真实 PTY 通过；安装器静态提取与 staging/ZIP 共 657 项文件逐字节一致。
- Mac：实际只读 DMG 与独立复制 App，版本/manifest、严格签名验证、原生模块、隔离配置、gRPC、GUI、宿主默认真实 PTY 及裸内核 PTY 通过；打包目录/DMG/副本共 536 项文件及符号链接内容一致。两份登录页截图由执行者亲自检查。
- 宿主 Ctrl+C 保留复制行为，使用显式关闭回收测试 PTY；Mac 裸内核单独验证 Ctrl+C 退出 0。Code Mode 关闭 HTTP/2 连接与 stdin 后正常退出，不把共同关闭的结果说成仅 EOF 的因果证明。
- 编译警告保留在验证记录，包括 Mac compact-unwind / `__eh_frame` 大小提示，不将成功退出描述为零警告。源码回归及各平台实际范围见 verification JSON。

## 下载与签名

- macOS arm64：`MultiAICode-2.2.7-arm64.dmg`；App 为临时签名（ad-hoc），没有 Developer ID 签名或 Apple 公证。
- Windows x64：`MultiAICode-2.2.7-x64.exe` 或 `.zip`；安装器与程序均为 **NotSigned**。
- 共十项资产：Mac DMG、blockmap、verification JSON、SHA256SUMS；Windows EXE、ZIP、blockmap、latest.yml、verification JSON、SHA256SUMS。文件大小、SHA-256 与详细验收证据以对应平台报告为准。

本轮没有运行 Windows 安装器或替换用户现装应用，也未使用真实账号发送模型或付费任务。完整登录后的主终端与真实 IM 任务链路、Intel Mac 与旧 macOS 实机运行未纳入本轮验收。

[源码集成与回归证据](https://github.com/wanhongqing123/multi-ai-code/blob/39e8eed575d9978535eba59a7a72a3a8a9ad1206/docs/diagnostics/codex-release-0172-integration-20261125.md) · [官方 0.172.0 发布说明](https://github.com/openai/codex/releases/tag/rust-v0.172.0)
