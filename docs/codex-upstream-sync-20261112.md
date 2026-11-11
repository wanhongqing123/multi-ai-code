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
- 额外交互式侧边会话用例曾一次在图片拖拽释放后观察到坐标 199/197 的差异；加入日志后通过，移除前置日志后又连续 3 次通过，且保存错误诊断未命中。原断言和栈预算未改，没有据此声称已证明该偶发差异的根因或修复了生产缺陷。随后冻结源码的 Windows 无插桩定向组 95/95 通过，正式优化构建继续执行。
- macOS 新优化版 Code Mode host 及正式 DMG 内副本都已用 V1 stdio 协议完成握手、建立会话、在 V8 执行 JavaScript 得到 42，并正常关闭。
- 相关原生回归的复现、诊断对照与修正边界见 `docs/diagnostics/codex-upstream-sync-native-regressions-20261112.md`。
- Windows 合作者核实并修正两处测试的 POSIX 路径假设，未修改生产路径；移除只验证已停用 elevated setup.exe 的集成测试，保留库测试。
- Mac 优化构建及安装包验收完成；Windows 正式产物和联合 GitHub 发布仍在执行中。用户运行中的旧版本未替换。

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
- 草稿现有正式 Mac 四项：DMG、blockmap、verification JSON、SHA256SUMS，均已 API 回读核对。Windows 95/95 无插桩、原 8 MiB 定向回归通过（4 leaky），不包括该平台不支持的 Unix socket 重连；Windows 正式优化编译及包内验收尚在执行，Release 仍未发布。
