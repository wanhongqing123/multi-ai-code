# Codex 上游同步与 2.2.6 发布

## 同步范围

- 主仓基线：`90e831360d9d36b25f1707772d0273feb0002895`，在 `main` 开发。
- Codex 子仓基线：`2165b1da63527b66f4e491ff5e5c96d763eab973`，维护分支 `multi-ai/im-bridge`。
- 官方上游固定到 `e58bf8f5233b51e7cd1679d947e3028c1ed91b06`，2026-11-11 的 `main`，新增 1,253 个提交。
- 子仓合并提交：`c118bb0c7fcbb3b427d19bc4f391d3f275c5d7e6`；最终冻结子仓为 `ac71f246052620472c2693d56a79ad5a1304f3d5`，均已推送。父仓完整冻结提交写入平台验收 manifest 和 Release。
- 采用 merge 保留定制历史，处理 23 个冲突文件。之前正式版回移的压缩、认证和 Windows 环境补丁采用当前上游实现；不再保留已经被上游重构取代的旧接口。

## 版本与兼容性

- 宿主版本：`2.2.6`。
- Codex 版本：`0.167.1+multi-ai.e58bf8f523`。同步时核对的最新正式标签是 `rust-v0.167.1`；本次同步的是更新的固定 `main`，因此以定制后缀明确标记来源，不能宣称是未经修改的正式标签内容。
- 消息转发使用上游 `ThreadItemPayload` 与生命周期 envelope 的规范入口，补齐新通知字段，保持原 IM 消息格式。
- 官方 `main` 的工作区版本为 `0.0.0`，不能直接带入安装包；同时更新工作区版本和锁文件，并校验真实二进制、manifest 与发布记录一致。sourceCommit 记录合并后的子仓提交，后缀记录上游基线，两者含义不同。
- 保留结构化 IM、逐条文本回复、异步提问、任务状态、错误转发、本地接管后的路由隔离、Windows 可选沙箱询问控制和显式危险模式。
- 上游把侧边会话创建改成异步：IM 的自动返回标记随创建状态保留，并在重试和复用时沿用当前入口的行为；新增交互式/IM 两种入口的回归用例。
- 上游把完成消息替换抽成独立入口：保留没有流式历史行时插入完整回复的行为，以及完成消息 ID 关联的诊断；同时采用上游中断片段与 phase 的处理。
- 保留原生终端光标策略及显式 JediTerm 修复开关，合入上游新增的图片重绘逻辑。
- 宿主同时包含已验证的图片下载有限重试，详细现场和未证明的网络底层原因见 `docs/diagnostics/multi-ai-code-macos-im-image-download-20261110.md`。

## 构建与验收安排

- Mac 现场磁盘空间有限且没有旧 Codex target 缓存；本地编译检查关闭调试信息与增量，只对当前构建子进程使用系统代理。源码回归完成后清理本轮测试 target，腾出空间在本机启动优化构建。
- macOS 本地优化构建使用 Rust 1.96.1、3 个 Cargo job，保留上游 release 的 thin LTO 和 4 个 codegen unit，仅关闭调试信息与增量。新安装包输出到独立目录，再从实际 DMG 做原生模块、版本和隔离启动验收。
- 新增 `build-macos-release.yml` 可按完整提交准备 arm64 包及资产摘要，先输出工作流产物，不自行发布 Release。本轮两个早期候选工作流在源码收敛前取消，正式 Mac 产物改为本机生成，不能将取消的工作流记为构建通过。
- `house-multi-ai-code` 负责 Windows x64 优化构建、EXE/ZIP 验收和指定草稿的资产上传。只有冻结提交之后生成的产物才可发布；不得把旧的 0.163.0 缓存当作新产物。
- 目标 Release：`electron-20261112`，仅包含本仓库 Multi-AI Code，不发布已拆仓的 MaiChat。

## 验证状态

- 宿主类型检查通过；修复 Windows CMD 的测试发现方式后，IM 回归共 48 文件。macOS 为 507 项通过、2 项按条件跳过；Windows 同一入口为 508 项通过、1 项跳过。
- 打包工具、产物齐全性与原生模块校验的 26 项测试通过。
- 冲突标记已清除；Rust 格式化已执行。上游自带的快照和 patch 空白保持原样，本地 Rust/TOML 差异单独进行空白检查。
- Codex CLI、TUI、Code Mode host 及其测试的 `cargo check --locked --tests` 在 macOS 和 Windows 均通过。
- macOS CLI/TUI/config 原生回归首次执行 6,906 项，其中 6,865 项通过、41 项失败，另有 5 项按条件跳过。失败用例在校准后逐项复验通过，已通过项目没有无故重复全跑。
- 其中 40 个失败用例的快照需要适应真实定制版号、保留的无沙箱选项，以及上游排版；后续断言继续暴露的快照也逐一复核，共校准 44 个文件。没有把 `0.0.0` 带入发布版或用过滤器隐藏真实版本。
- PTY 启动回归在相同二进制下以 `TERM=dumb` 失败、以 `xterm-256color` 通过，测试只为其终端子进程设置明确的终端能力，未增加超时。
- 大型侧边会话和重连测试使用项目已有的非内联 `boxed_future!` 工厂隔离 poll frame；macOS 三个相关用例在原 8 MiB 栈预算下通过。32 MiB 只作诊断对照，未进入正式构建或测试预算。
- Windows 定向原生回归初次为 89/92 通过，另三项为两条权限菜单快照和 IM 测试栈溢出；堆分配边界已由 Windows 同场景在原 8 MiB 下验证红转绿，权限用例包括 Enter/fallback 后续断言通过。
- 额外交互式侧边会话用例曾一次在图片拖拽释放后观察到坐标 199/197 的差异；加入日志后通过，移除前置日志后又连续 3 次通过，且保存错误诊断未命中。原断言和栈预算未改，没有据此声称已证明该偶发差异的根因或修复了生产缺陷。随后冻结源码的 Windows 无插桩定向组 95/95 通过，正式优化构建及安装包验收也已完成。
- macOS 新优化版 Code Mode host 及正式 DMG 内副本都已用 V1 stdio 协议完成握手、建立会话、在 V8 执行 JavaScript 得到 42，并正常关闭。
- 相关原生回归的复现、诊断对照与修正边界见 `docs/diagnostics/codex-upstream-sync-native-regressions-20261112.md`。
- Windows 合作者核实并修正两处测试的 POSIX 路径假设，未修改生产路径；移除只验证已停用 elevated setup.exe 的集成测试，保留库测试。
- 两端优化构建、包内验收和联合 GitHub 发布均已完成。验收使用隔离、未登录环境；完整登录后的主终端与真实 IM / 模型任务链路未复验，用户运行中的旧版本未替换。

## 冻结源码和 macOS 安装包

- 冻结主仓：`21f97ee4f6a23866cfd73f37fb208e2fd4afa9b2`；冻结子仓：`ac71f246052620472c2693d56a79ad5a1304f3d5`。后续验收记录的文档更新不改变该构建来源。
- macOS 27.2 / arm64 本机正式 release 构建退出 0，耗时 24 分 45 秒。打包输出 `.cache/release-2.2.6-macos-20261112/`，与旧包目录独立。
- `MultiAICode-2.2.6-arm64.dmg`：296,859,371 B；SHA-256 `647070f50aa36b387afb29674593d68780111f5f2bf3d37caf1e2d8530bfeab4`。
- 挂载该只读 DMG，核对宿主 2.2.6、真实 `codex-cli 0.167.1+multi-ai.e58bf8f523` 和 manifest 子仓完整提交；包内主进程与刚完成的生产构建摘要一致。
- DMG 校验、应用 ad-hoc deep/strict 签名校验、包内 Electron ABI 130 的 SQLite 3.49.2 / node-pty 加载均通过；不是 Developer ID 公证包。
- DMG 内 Code Mode host 通过真实 V1 stdio / V8 执行得到 42，正常关闭。
- 从 DMG 启动全新数据与 Chromium profile，实际登录页绘制正常，preload 和版本 IPC 正常，前端异常数 0，测试进程退出 0；没有绑定真实 IM 账号。
- 用新包自己的 Electron / node-pty 拉起与 DMG 摘要完全一致的内核，实际首启显示完整版本与登录菜单，取消后退出 0；临时 CODEX_HOME 与用户数据隔离。
- GitHub 草稿 ID `442021899`，tag `electron-20261112`，target 为冻结主仓。DMG 直传遇到 EOF 后采用 18 个 16 MiB 分段，通过既有工作流 `43074437596` 校验合并；完整文件 API size/digest 与本地一致后删除所有分段。
- 正式 Mac 四项：DMG、blockmap、verification JSON、SHA256SUMS，均已 API 回读核对。Windows 95/95 无插桩、原 8 MiB 定向回归通过（4 leaky），不包括该平台不支持的 Unix socket 重连。

## Windows 安装包

- `house-multi-ai-code` 在 Windows 11 x64 完成实际构建与验收。Rust 1.95.0、VS2022 / MSVC 14.44.35207、2 个 Cargo job，保持源码 release 的 thin LTO / 4 codegen unit / line-tables-only 配置；`cargo build --locked --release --bin codex --bin codex-code-mode-host` 退出 0，耗时 63 分 54 秒。
- `MultiAICode-2.2.6-x64.exe`：259,867,569 B；SHA-256 `19669303a2415ac5cc6c8a237409801b14061cba4fcdd6376a0a795b243afe9f`。
- `MultiAICode-2.2.6-x64.zip`：351,823,668 B；SHA-256 `09b3f072d28137551c123306e244eb3396d45dbde1974e24ddf2671e28dc8a37`。
- staging 和实际 ZIP 解压副本都核对真实宿主 2.2.6、`codex-cli 0.167.1+multi-ai.e58bf8f523`、manifest 完整 ac71 提交，拒绝旧 0.163.0 缓存。
- 两个目录的 21 个 PE 文件在受控裸 PATH 下审计，无缺失 DLL 或仅由开发机 VC 运行库提供的依赖；包内 Electron 33.4.11 / ABI 130 真加载 SQLite 3.49.2、node-pty、koffi 3.1.5 和 TRTC。
- 两个目录内 Code Mode V1 实际执行返回 42、正常退出；全新 root / profile 的实际登录窗口截图由 Windows 执行者亲自检查，无未捕获异常；宿主 IPC 的 cmd marker、包内 Codex 版本 PTY 均退出 0。另以独立 ConPTY 实开内核，看到正确版本与三项登录菜单，取消后退出 0，所有本轮 QA 进程已退出。
- electron-builder 在 NSIS 后置加入 `resources/elevate.exe` 安装维护工具，最初 ZIP 未包含该文件。本轮从同一 staging 补齐相同的 107,520 B 文件，重新解压最终 ZIP；安装器静态提取、staging 和最终 ZIP 的 657 个文件 / 1,023,072,984 B 逐字节一致。
- Windows 安装器和程序均为 NotSigned。没有执行安装器或覆盖现有安装；未连接真实账号、发送模型请求或验证完整登录后的主终端和真实 IM 链路。TRTC / koffi 证明范围为原生加载 / 系统调用，未验证真实远程控制会话。
- Windows 六项：EXE、ZIP、EXE blockmap、latest.yml、verification JSON、SHA256SUMS；均直接上传成功，API size/digest 与本地上传前后重算一致，latest.yml 的 SHA-512 / size 与该 EXE 一致。

## 正式发布

- [Multi-AI Code 2.2.6 / electron-20261112](https://github.com/wanhongqing123/multi-ai-code/releases/tag/electron-20261112) 已于 2026-11-12 03:18:30 CST 发布，并设为 Latest。
- 发布后 API 回读：`draft=false`、`prerelease=false`、10/10 正式资产为 uploaded；所有 size / SHA-256 与已验收的本地文件或 Windows 验收记录一致，没有遗留分段资产。
- GitHub 标签实际指向 `21f97ee4f6a23866cfd73f37fb208e2fd4afa9b2`，子仓固定为 `ac71f246052620472c2693d56a79ad5a1304f3d5`。后续提交仅补充验收文档，不改变已发布二进制的源码身份。
- 发布前协调者重新下载并校验 Windows verification / SHA256SUMS / latest.yml，逐项核对两个目录内版本、原生模块、Code Mode、GUI、PTY、首启、进程清理和安装器 / ZIP 内容一致性；同时重算 Mac 四项摘要并与 GitHub 10 项资产交叉核对。
