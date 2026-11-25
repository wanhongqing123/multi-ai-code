# Multi-AI Code 2.2.7

本轮为 Windows x64 / macOS arm64 联合发布候选。两端产物验收、摘要回读后统一发布，
不能把以下已完成的源码回归视为正式包已经验收。

## Changed

- Codex 核心同步到官方 `rust-v0.172.0` 正式发布，定制版号
  `0.172.0+multi-ai.3d86e43b07`。保留结构化 IM、逐条回复、审批、侧会话自动返回及宿主主题控制。
- 宿主继续使用内嵌 app-server；正常交互、resume/fork/agents 显式选择该入口，
  保留用户主动指定的远端及服务命令，不要求为普通宿主会话安装后台 daemon。
- Code Mode 跟随上游使用 gRPC 执行通道；验证改为真实会话、本地 V8 计算与自然退出，
  旧 V1 JSON stdio 探针不适用于本版本。

## Fixed

- 适配远端服务版本可缺省的新接口，IM 状态在版本未知时省略后缀。
- 使用已有非内联堆分配边界收敛 Windows 大型事件 future 的构造留栈，保持 8 MiB
  栈预算、事件处理顺序和断言。dev 产物入口留栈已实测缩小，不能据此宣称所有生产路径栈安全。
- 保留原生 Windows 路径的确认框快照，避免渲染换行后再做整串路径替换导致误判。

## Build and validation

- 宿主：2.2.7；Codex 子仓精确提交：`6e09a1b58f759848942e3d5b7653ab714f045ada`。
- 官方基线：`3d86e43b075adeba2193ec5c22ac92f55afd82e6`；构建使用 Rust 1.99.1。
- 版本冻结前源码回归：Windows 原生 208/208（7 leaky）、宿主 IM 508 通过 / 1 跳过、
  启动参数与 PTY 111/111、打包工具 26/26、完整类型检查通过。
- Windows 安装器与 ZIP、Mac DMG 均从相同冻结父仓/gitlink 独立构建；实际包内版本、
  manifest、原生模块、Code Mode、隔离 GUI 与真实内核 PTY 由各平台执行者验收。
- 正式平台文件名、大小、SHA-256 和实际验证范围以 Release 中的平台 verification JSON
  与 SHA256SUMS 为准；未完成包验收的资产不得发布。
- 不运行 Windows 安装器或替换用户现装应用。不会通过本轮发布自动迁移/替换正在运行的实例。

## Boundaries

本轮不使用真实账号触发模型或付费任务；完整登录后主终端及真实 IM 任务链路不以本地
fixture 测试冒称已验证。Windows 代码签名、Mac Developer ID / 公证状态按实际包记录，
未签名/临时签名不能标作正式开发者签名。

源码集成和红转绿证据见
[集成排障记录](../diagnostics/codex-release-0172-integration-20261125.md)。
上游改动详见 [官方 0.172.0 发布说明](https://github.com/openai/codex/releases/tag/rust-v0.172.0)。
