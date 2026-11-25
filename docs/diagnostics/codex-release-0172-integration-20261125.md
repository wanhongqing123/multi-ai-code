# Codex 0.172.0 正式发布线同步

## 任务、基线与预期

2026-11-25，用户要求同步本仓 Codex 源码到上游最新发布分支。
本轮只做源码集成与 Windows 本机验证，不发布安装器或替换现用应用。

- 父仓开始为 `21f97ee4f6a23866cfd73f37fb208e2fd4afa9b2`；先快进到
  `444c13365`，中间仅有上一轮发布验收文档更新。
- 子仓开始为 `ac71f246052620472c2693d56a79ad5a1304f3d5`，继续使用既有
  `multi-ai/im-bridge` 分支，采用 merge，不重写已推送历史。
- 原有未跟踪的 MaiAgent / MaiChat 数据及 2026-11-12 Windows 验收记录保持原状。

## 官方发布身份

- [官方最新正式发布](https://github.com/openai/codex/releases/tag/rust-v0.172.0)：
  `rust-v0.172.0`，`prerelease=false`，发布于 `2026-11-24T00:03:55Z`。
- Annotated tag object：`fc3036dba44468096953f0e832f3fecd3adc0228`。
- 实际发布提交：`3d86e43b075adeba2193ec5c22ac92f55afd82e6`。
- 对应 `release/0.172.0-alpha.2-b9dab9ed` 分支停在父提交
  `14314df0799ce1d02d90c40145b2aa1dbe617ca6`；正式标签还包含发布提交，
  因此没有把 alpha 分支 HEAD 当成最终发布源码。
- 核验时 upstream/main 为 `ebd0765aafa255d63ebf690ebdffe0164dc14a0c`，
  相对正式标签多 341、少 3 个提交；没有混入该 main 或 0.174 alpha。
- 新 Codex 工作区版本为 `0.172.0+multi-ai.3d86e43b07`；后缀标识定制来源，
  不能称为未经修改的官方原版。宿主仍为 2.2.6，本轮不产出同版本新安装包。

## 合并决策

此次有 471 个未合入的上游提交，产生 12 个文本冲突文件。

- Cargo：保留正式发布的依赖锁定图，仅更新继承工作区版本的 path 包。
  官方锁文件中部分新包仍为 0.0.0；Cargo 还规范化了五个未显式列在 members
  中的工作区 path 包。用离线最小解析确认仅这五处版本变化后，恢复 `--locked`
  检查，未执行全量依赖更新。
- CLI doctor：同时保留定制的 helper 未打包诊断，以及上游新增的 service timeout /
  pipe closed 诊断分支。
- 配置测试：保留显式危险模式独立状态的断言，并适配新的 `ConfigLoadOptions`；
  上游新增的 status-line 配置诊断测试均保留。
- App：保留上游 backend registry 与本地 `Box<ChatWidget>`；新增测试中的后台语音
  替换点移动既有 Box，避免双重装箱。IM `/clear` 使用新的
  `FreshSessionStart::PreserveCurrentView` 接口。
- 完成消息修正：保留上游隐藏回复清理、占位匹配和 cloud copy 修复；保留本地
  消息 ID / 长度 / 历史单元数量诊断，并补全隐藏与删除分支的诊断状态。
- ChatWidget：thinking 路由 helper、逐条 IM 去重与上游按 turn/item 切换流、cloud
  copy 来源修正并存；状态文本保留 IM 纯文本入口并采用上游新模型展示入口。
- Terminal：保留上游按 terminal ID 复用和移动图片的重绘流程、alternate-scroll
  探测、panic/外部编辑恢复；Native 光标默认与 JediTerm 显式 opt-in 继续保留。

独立只读审阅跟通了显式 bypass 的配置传递和新 prepared approval 接口、elevated
拒绝与 helper 关闭、IM 任务路由和侧会话 completion 自动返回；未发现有证据的本轮
新增语义丢失。既有 ThreadSnapshot 路由的潜在边界没有借本次合并擅自修改。

## 环境和验收入口

- Windows x64 / MSVC 14.44，按上游安装 Rust 1.99.1
  (`58cf8f9c3 2026-10-13`)，未改变默认工具链或运行中产品。
- Cargo 两个 job；本地 dev/test 关闭 debug 信息和 incremental，正式栈保持 8 MiB。
- V8 仍为固定 152.2.0；通过上游校验入口复用已验证 archive 与 binding。
- 全仓 `cargo fmt --all` 在 Windows 命令行长度上限处失败，改为逐个指定本次修改的
  11 个 Rust 文件，以 `skip_children=true` 做格式检查，退出 0；没有重排无关上游文件。
  稳定版忽略 `imports_granularity` 的警告与上游 rustfmt.toml 注释一致。
- 宿主类型检查通过；IM 回归为 48 文件、508 通过、1 跳过；打包工具使用 Vitest
  正确入口执行，为 3 文件、26 通过。误用 Node test runner 的结果不计入验收。
- Code Mode 已迁移为 gRPC-only。旧 V1 JSON stdio 探针不用于本轮证明。
  使用 `code_mode_host::tests::grpc_stdio_sessions_share_a_host_and_isolate_state`
  验证本地 V8 42/会话隔离，使用
  `code_mode_host::tests::default_host_serves_grpc_and_exits_when_stdin_closes`
  验证自然退出；显式指定本轮 `CARGO_BIN_EXE_codex-code-mode-host`。
  两者仅通过子进程管道通信，不访问真实账号或模型。

## 实际运行发现与修正

### 嵌入式宿主与上游默认 daemon

新普通 CLI `--version` 正确，但全新 home 首启退出 1，提示
`this CLI has no complete local package`，入口为
`app-server-daemon/src/prepare_install.rs:149`。上游默认连接/安装后台 daemon，
而本产品沿用内嵌 app-server、只打包 kernel 与 Code Mode host，不能把裸构建的
退出当作网络或账号问题。

相同产物使用官方 `--no-daemon` 后，实际 ConPTY 显示准确版本及三项登录菜单，
Ctrl+C 退出 0。宿主在 `withEmbeddedCodexArgs` 中对普通交互、resume/fork/agents
显式选择内嵌入口；不覆盖 `--remote`、已有选择、帮助/版本及服务子命令，不改动
调用者数组或 sandbox/approval 参数。PTY 边界回归先验证缺少参数的失败，再验证
修正后的实际 spawn 参数；111 项相关测试通过。

### 新接口的兼容适配

- `RemoteConnectionStatus.version` 改为 `Option<String>`，首次编译以 E0277
  指向定制的 IM 状态格式化。沿用上游显示规则：已知版本保留，缺省版本省略后缀；
  新增 IM 输出 Some/None 两种情况的断言。
- 新侧会话测试显式保留交互入口的 `auto_return_on_turn_complete=false`。
- 旧 IM 输入回显测试补齐 live/confirmed/history 状态字段，并将 cwd 转为 `PathUri`。
- 初次手工 Cargo 检查未传校验后的 V8 路径，误触发 denoland 下载并获得 404；
  改为通过本仓 resolver 向每个 Cargo 子进程传入 archive/binding，未修改 V8 版本。

### 8 MiB 栈边界

补充展示组中 `opens_agent_transcript_beside_parent_and_returns_to_chooser` 在
8 MiB 下两次以 `0xc00000fd` 中止。CDB 原产物栈为有限调用链：测试场景 →
`handle_event` → `open_sidebar_agent` → `select_agent_thread` →
`attach_live_thread_for_selection` → `prepare_saved` → `resume_thread` →
`thread_read` / `__chkstk`，未见无限递归。

测试场景 poll 栈帧约 3,822,968 B，嵌套事件 poll 栈帧约 3,803,168 B；
恢复路径继续压栈后越界。普通非测试 dev CLI 的 `App::run` 也独立测得
3,528,248 B 留栈，不能把 cfg(test) 产物尺寸直接当成普通 CLI 尺寸。

使用项目已有非内联 `boxed_future!` 工厂，收敛该测试 25 个事件构造点，以及
`app/startup.rs` 三个生产事件入口；顺序 await、原断言、超时和正式栈预算不变。
原 8 MiB 定向用例通过，Python 明确捕获子进程退出 0。
原先一轮 PowerShell 外层退出 1 不能归因于 leaky 策略，后续明确捕获已排除该歧义。

普通 CLI 相同 dev 配置重建后，CDB 在初始断点只读检查副本的 prologue 与 PE unwind：
`App::run` 留栈由 3,528,248 B 降到 3,123,032 B，减少 405,216 B。
这证明该入口构造帧缩小，不代表所有生产调用路径或优化发行版都已证明栈安全。
32 MiB 仅用于原测试产物诊断，没有进入正式配置。

关键证据位于 `.cache/codex-0172-sidebar-stack-red-cdb.log`、
`codex-0172-sidebar-production-focused.log`、
`codex-0172-sidebar-production-build.log` 和
`codex-0172-sidebar-frame-comparison.json`。

### UI 快照与 Windows 路径

44 份原有快照仅调整真实版本及其引起的占位/换行，保留原 provenance header，
未用过滤器隐藏版本，也未接受与版本无关的行为变化。
补充组第一次 69 项为 46 通过、23 失败（包括上述栈用例及版本快照）。

组合组后来 207/208 通过，仅 daemon 确认框失败：测试在渲染后替换 Windows 完整
路径，但原生路径已先在 `cli-build` 的连字符处分行，替换无法匹配。
没有扩大窗口或放宽断言；该原生显示采用独立 Windows golden，保持其他平台基线，
保留全部取消、零副作用和 handoff 断言。

## 最终本机结果

- 子仓合并提交 `10db33837d168c68a70b89c7bc4e25e1f5d0b395` 的双亲为原
  `ac71f246` 与官方 `3d86e43b`；后续边界/快照提交为 `860a706673f66fb3004bf9891561c72b0ac241ce`。
- 最终子仓：`6e09a1b58f759848942e3d5b7653ab714f045ada`。
- `cargo check --locked --tests` 覆盖 CLI、TUI、Code Mode host、config、core、
  Windows sandbox，通过；保留两处 IM 包装函数 dead_code 警告。
- 相同源码普通 dev 构建退出 0，初次完整构建 11m43s；最后在干净提交上重跑缓存
  构建退出 0（1.45s）。本轮没有构建或发布优化安装包。
- 干净最终提交的原 8 MiB / 4 测试线程组合组为 **208/208 通过（7 leaky，
  10149 未选中）**，覆盖 IM、审批、侧会话、状态/排版、相关旧操作以及 gRPC 本地执行。
  首个 126 项组曾为 126/126（6 leaky）；不把后续失败描述成首次全量一次通过。
- Code Mode 新 gRPC 测试实际启动本轮 host，返回 42、隔离两会话，并验证关闭
  stdin 后自然退出；不再使用旧 V1 JSON stdio 探针。
- 主仓完整类型检查通过；宿主 IM 为 508 通过、1 跳过；启动/PTY 111 项通过；
  打包工具 26 项通过。
- raw kernel 和 host 的 PE 检查无缺失 DLL 或仅依赖开发机 VC runtime 的项目。
  实际版号为 `codex-cli 0.172.0+multi-ai.3d86e43b07`。
- 新普通 CLI 在隔离 home 与裸 PATH 下，用 `--no-daemon --no-alt-screen` 实际
  首启到三项登录菜单，取消后退出 0；测试过程中未登录真实账号或提交模型任务。

最终日志：`.cache/codex-0172-final-native.log`、`codex-0172-final-clean-build.log`、
`codex-0172-final-raw-pe.json`、`codex-0172-final-version.log`、
`codex-0172-host-typecheck-final.log`、`codex-0172-host-im-final.log`、
`codex-0172-launch-tests.log`。快照失败输出另归档到 `.cache/snap-0172-red/`；
框架会自动删除已通过断言对应的 `.snap.new`，相关字节恢复/诊断不能替代最终源码回归。

## 证明边界

验证平台为本机 Windows；其他平台未作原生构建，优化发行包、真实账号/IM 发送、
模型请求和完整登录后交互未验证。208 是明确筛选的组合组，不是全部上游测试。
leaky 是需要保留的测试清理告警，不声称为零警告或已修复其来源。
已安装/运行中的应用、旧产物和用户数据没有被替换；宿主未递增版本、未生成安装器、
未打标签或发布 GitHub Release。后续出包需先确定新宿主版本及冻结提交。
