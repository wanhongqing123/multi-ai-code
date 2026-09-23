# Codex 上游同步（2026-09-23）

## 同步范围

- 上游：`openai/codex` 的 `main`，固定到 `39598ed178`（Support Shift-click to extend transcript selections）。
- 同步前：`9bb4dbddec`，分支 `multi-ai/im-bridge`。
- 合并提交：`ce6d4802a56befc34549da5de6f0718eb2ab86e0`，已推送到该分支。
- 合入上游新增 1,086 个提交，保留本地 13 个定制提交的历史，采用 merge，不重写已推送历史。
- 处理 24 个冲突文件，主仓库随之更新子模块引用。

## 保留与适配

保留结构化 IM 通道、逐条回复、工作状态、异步提问、审批关联、错误转发、本地接管后的路由隔离，以及宿主的 Windows 可选沙箱提示策略。

适配上游的新启动入口、输入来源类型、服务器侧 Windows 沙箱配置和消息合并接口。配置解析器补齐上游新增的 `mxc` 模式；继续明确拒绝本构建不支持的 `elevated`，并重新生成配置 schema。采用上游的新复制行为：任务进行中已完成的说明文字也可以复制，不继续指向上一轮回复。

上游状态对象增大触发了本地已有的 64 KiB 栈大小保护。将主聊天组件放在堆上后，`App` 从 82,344 字节降为 37,192 字节；保持原断言阈值。前后台语音组件切换仍交换同一个组件的所有权。

嵌入终端保持原生光标命令为默认行为，JediTerm 修复继续由 `CODEX_JEDITERM_CURSOR_REPAIR` 显式启用。测试用显式模式覆盖两条路径，并保留避免重复发送 show-cursor 的行为。

## 可重复验证

在 Multi-AI Code 内运行测试时，宿主可能注入真实会话的配置/数据库目录及终端颜色。macOS 系统代理也可能接管本机 mock server 请求。这些条件会影响数据库、颜色快照和本地 HTTP 用例，不能直接当作产品回归。

本仓库新增隔离入口：

```sh
python3 scripts/test-codex-local.py -p codex-tui --test-threads 8
python3 scripts/test-codex-local.py -p codex-config --lib
python3 scripts/test-codex-local.py -p codex-cli --bin codex
python3 scripts/test-codex-local.py -p codex-core --lib -E 'test(exec_policy) | test(config::)'
```

脚本为本轮测试创建临时 `CODEX_HOME`，移除宿主数据库、会话、颜色及模型凭证注入，补充 loopback 的 `NO_PROXY`。它只修改子进程环境，不修改系统网络配置或用户设置。

基线对比必须使用独立构建缓存；Cargo 的工作区辅助库会嵌入源码/fixture 路径，共用 target 可能读到另一份工作树的资源。

## 验证结果

- TUI 完整回归：5,512 项运行，5,509 项通过；3 个时序/启动用例随后单独复测通过。首次运行另有 8 项按仓库条件跳过。
- IM、路由、Windows 配置、光标及栈保护兼容用例：61/61 通过。
- 配置库：348/348 通过。
- CLI 单元测试：307/307 通过，1 项跳过。
- 核心配置与执行策略：623/623 通过（限定测试范围，没有运行完整工作区套件）。
- `just bazel-lock-update` 已执行，锁文件与上游一致。
- `just write-config-schema` 已执行，并包含本地支持的 Windows 模式变化。
- `just fix`、最终 `just clippy` 和 `just fmt` 均完成；lint 保留既有未使用兼容函数及参数数量告警，无错误。

此前受宿主配置、系统代理及复用基线编译缓存影响的结果不作为最终验收数据。代码未修改测试时间阈值，也没有用放宽栈大小断言解决失败。

此次更新范围是源码与子模块引用，Windows 真机和安装包验证不包含在本轮本地测试内。
