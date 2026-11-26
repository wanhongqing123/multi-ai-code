# Multi-AI Code 2.2.7 Windows 发行构建与本机验收记录

记录日期：2026-11-25–26，Asia/Shanghai。Windows 与 Mac 正式资产均已验收并公开发布。
本机 Windows 验收汇总于 `2026-11-25T13:15:15.011Z`
（北京时间 21:15:15）通过。Windows 六项已于北京时间 21:17:59 独立远端回读一致。
**联合 Release 已于 2026-11-26 16:15:20 CST 正式发布并设为 Latest。**
[下载 Multi-AI Code 2.2.7](https://github.com/wanhongqing123/multi-ai-code/releases/tag/electron-20261125)。

文中 `.cache/` 日志是执行者保留的本机证据，不随仓库分发；六项资产的公开 verification
与 SHA256SUMS 已随指定联合 Release 发布。CI 日志可从文中相应 Actions run 链接查询。

## 冻结源码、版本与实际产物

| 项目 | 本轮实际值 |
| --- | --- |
| 父仓冻结提交 | `39e8eed575d9978535eba59a7a72a3a8a9ad1206` |
| Codex 子仓冻结提交 | `6e09a1b58f759848942e3d5b7653ab714f045ada` |
| 父仓 gitlink | `160000 commit 6e09a1b58f759848942e3d5b7653ab714f045ada third_party/aicli/codex` |
| 宿主 | `2.2.7` |
| 内核实际版本输出 | `codex-cli 0.172.0+multi-ai.3d86e43b07` |
| Electron / Node ABI | `33.4.11` / `130` |
| 平台 | 本机物理 Windows x64；没有运行干净 VM 或多版本 Windows 矩阵 |
| 发行目标 | tag `electron-20261125`，release ID `458049005`；draft=false、prerelease=false、Latest，公开核对通过 |
| 产物根目录 | `.cache/electron-20261125-win32-x64-39e8eed5/` |
| staging | 上述根目录的 `win-unpacked/` |
| 最终 ZIP 提取目录 | 上述根目录的 `zip-extracted/` |
| 安装器静态提取目录 | 上述根目录的 `installer-extracted/` |

本机 gate 与最终汇总均重新读取父/子仓 HEAD、父仓结构化 gitlink，并检查 tracked tree 干净；
已观察到的包内、raw release、native 模块及探针身份在运行前后保持一致，汇总前重新计算 SHA256。
macOS 工作流定义修正提交 `1764add23ef848451eff1e3382684b0b7af353d9` 不改变本轮 Windows
源码冻结身份；正式构建和验收期间没有用远端后续 HEAD 替代 `39e8eed5`。
Windows 验收、上传完成后，本机仅为同步工作流与记录文档快进到 `c28b1218`；已上传资产没有重编或重写。

## 发行构建

使用 Rust `1.99.1`（`58cf8f9c3 2026-10-13`），目标为
`x86_64-pc-windows-msvc`，LLVM `23.1.1`，Cargo 两个 job。
源码中的 release profile 原样生效：`lto=thin`、`debug=line-tables-only`、
`split-debuginfo=off`、`strip=false`、`codegen-units=4`，没有 release profile 环境覆盖。

正式命令为：

```text
cargo build --locked --release --bin codex --bin codex-code-mode-host
```

编译耗时 **69 分 38 秒，退出 0**。随后通过仓库入口
`node scripts/build-aicli-codex.mjs` 完成正式构建/拷贝，退出 **0**；该入口日志中的缓存
release 构建耗时 22.50 秒。保留了两条 IM wrapper `dead_code` 警告，未将构建描述为零警告。
V8 使用仓库 resolver 校验过的 `152.2.0` archive/binding，未替换 V8 版本。

证据：`.cache/release-227-kernel-build.log`、`release-227-kernel-compile.json`、
`release-227-kernel-copy.log`、`release-227-kernel-copy.json`、`release-227-package.log`。

## 本机回归结果

| 检查 | 实际结果与范围 |
| --- | --- |
| 冻结子仓 native 选择集 | 208 通过，7 条 nextest leaky 警告，10149 未选中；原 8 MiB 栈预算 |
| 宿主 IM | 48 个测试文件，508 通过，1 跳过 |
| 打包与启动/PTY | 7 个测试文件，137 通过；其中打包/native 检查 26、runtime/PTY 检查 111 |
| 宿主类型检查 | node/web 两个 TypeScript 配置通过；执行者记录命令退出 0 |
| 行宽检查 | 通过 |
| Electron 构建 | main、preload、renderer 三个 bundle 均构建完成 |

native 选择集来自干净的 `6e09a1b5`，不是全部上游测试，7 条 leaky 清理警告保留为已知边界，
没有声称已定位或修复其来源。日志为 `.cache/codex-0172-final-native.log`、
`release-227-im-tests.log`、`release-227-package-launch-tests.log`、
`release-227-typecheck.log`、`release-227-line-width.log`、`release-227-host-build.log`。

## staging、安装器与最终 ZIP 完整性

安装器使用仓库固定的 `node_modules/7zip-bin/win/x64/7za.exe` 静态提取，**未运行安装器**。
实际安装器被 7za 识别为直接列出应用文件的嵌入式平铺 `Type=7z`：

- offset：186081 字节；physical size：265522435 字节；tail size：402441 字节。
- 三者合计 266110957 字节，与最终 EXE 大小一致。
- `There are data after the end of archive` 为本次 7za 列表输出中的已观察警告，列表命令退出 0；
  未把不存在的 `$PLUGINSDIR/app-64.7z` 当作此安装器的实际结构。

打包日志显示 ZIP 先生成，随后 NSIS 步骤向 staging 加入 `resources/elevate.exe`，因此最初 ZIP
缺少该 helper。root 将 staging 中**完全相同**的文件追加到最终 ZIP，再重新提取并完整比较。
追加文件为 107520 字节，SHA256：
`9b1fbf0c11c520ae714af8aa9af12cfd48503eedecd7398d8992ee94d1b4dc37`。
这是已知打包顺序的发行产物收敛操作，本轮未修改打包器源码。

| 对比 | 文件数 | 总字节 | 缺失 / 多余 / 内容差异 |
| --- | ---: | ---: | --- |
| staging 对静态提取安装器 | 657 | 1,057,627,055 | 0 / 0 / 0 |
| staging 对最终 ZIP 提取目录 | 657 | 1,057,627,055 | 0 / 0 / 0 |

文件数由实际枚举得出，逐一比较 SHA256；未用历史固定数量代替枚举。
两份 payload JSON 同时记录容器在提取前的 path/size/SHA256，并在提取和比较后确认未改变；
最终汇总将其绑定到上传清单中的实际 EXE/ZIP。证据为
`.cache/release-227-installer-payload.json`、`release-227-zip-payload.json`。

`latest.yml` 的版本、installer URL/path、大小及两处 SHA512 均与当前 EXE 匹配。
实际安装器 Authenticode 状态为 **`NotSigned`**；不能从日志中的“signing with signtool”文字
推断已经签名，实际打包日志随后记录没有签名信息并跳过签名。

## 两份实际应用目录上的 probes 与 gRPC

staging 和最终 ZIP 提取目录分别完整运行以下正式 gate，均为 passed；早期 raw/self-test
结果没有冒充这两轮正式包验收。

| 项目 | 两份目录中的实际结果 |
| --- | --- |
| 版本与 manifest | ASAR 宿主 2.2.7、PE 版本、真实 CLI 输出和 manifest 的版本/平台/源码/路径一致 |
| PE 与 DLL | 每份目录检查 21 个 PE loader context；缺失 DLL 为 0、仅靠宿主 VC runtime 解析为 0；无开发 PATH 回退 |
| 配置行为 | default / empty / unelevated 返回 `Not logged in`，退出 1；disabled / unknown / elevated 按设计拒绝，退出 1 |
| Electron native ABI | Electron 33.4.11 / ABI 130；SQLite 3.49.2 查询通过，node-pty、koffi 3.1.5 系统调用及 TRTC native load 通过 |
| 官方 Rust gRPC client | 两个精确 core test 通过；局部 V8 42、会话隔离、关闭第一会话后第二会话仍可用、stdin EOF 后 helper 正常退出 0 |
| 独立 Node gRPC client | 同一 host 的两个 session、V8 `42`、隔离结果 `undefined`、关闭第一会话后第二会话仍返回 `42`；client close + stdin EOF 后 helper 自然退出 0 |

官方 Rust 精确用例为：

```text
code_mode_host::tests::grpc_stdio_sessions_share_a_host_and_isolate_state
code_mode_host::tests::default_host_serves_grpc_and_exits_when_stdin_closes
```

Rust 的 `42` 来自测试内断言，不要求 harness 在 stdout 打印结果。Rust 用例未在 Windows 上独立
断言共用 host 的 spawn 次数；“同一 host 两会话”的独立证明来自 Node client。
两种 client 均使用当前 HTTP/2 gRPC over child stdin/stdout，没有使用旧 V1 JSON stdio 探针。
Rust 独立覆盖 stdin EOF；Node 同时关闭 HTTP/2 client 并发送 stdin EOF，不能据此把 EOF
宣称为 Node helper 关闭的唯一原因，也不能据此解释此前其他 EOF 事故的根因。

gRPC gate 的 binary before/after 身份和最终磁盘 SHA256 一致；Rust 编译 core test 的源码清洁与
编译来源由执行者结合冻结源码、native 日志及 core test 身份补充证明，未把单独协议探针的
`sourceCleanVerifiedByThisProbe=false` 误写成该探针自行验证了源码清洁。
正式证据为 `.cache/release-227-{stage,zip}-verification.json`、
`release-227-{stage,zip}-grpc-rust.json`、`release-227-{stage,zip}-grpc-node.json`
及各自的 `.envelope.json`。

## Ctrl+C 验收假设失误与按产品契约重新验收

复现步骤：在新 QA 数据目录启动 staging 包，真实 renderer 通过宿主 IPC 创建 cmd marker、
绝对路径 Codex `--version`，以及裸 `codex --no-alt-screen` 交互 PTY；等待准确版本和完整
三项登录菜单，随后发送 Ctrl+C。原验收脚本错误预期“Ctrl+C 后 CLI 自然退出 0”。

现场第一次运行时间为北京时间 **21:08:44–21:09:09**。准确版本和全部登录选项已经出现，
但交互 PTY 没有按该假设退出；报告为 `Interactive PTY did not exit after Ctrl+C`，之后的
清理才通过 `cc:kill` 结束 PTY，保留失败报告而没有将其改为通过：
`.cache/release-227-stage-ui-ctrlc-assumption-failed.json`、同名 `.log` / `.png`。

直接源码证据位于 [ptyManager.ts](../../electron/cc/ptyManager.ts#L297) 附近的
`shouldSuppressMainTuiCtrlC`：Windows/macOS 主 TUI 将独立 `\x03` 保留给复制并阻止其成为
PTY interrupt；[平台断言](../../tests/electron/cc/ptyManager.test.ts#L644) 保留对应预期。
现场 lifecycle 的 `lastEtxAt`、`lastInputAt` 都为 null，与该现有产品契约一致。
**此失败属于 QA 假设错误，不是产品回归；未为此修改或“修复”产品源码。修复提交不适用。**

只修改忽略的 QA harness 与汇总判定，改为：

1. 裸 `codex` 仅由 caller 提供 `--no-alt-screen`，嵌入式 runtime 参数来自实际宿主。
2. 必须看到精确版本及 `Sign in with ChatGPT`、`Sign in with Device Code`、
   `Sign in another way` 三项菜单；不选择登录方式、不按 Enter、不提交 prompt。
3. 发送一次 Ctrl+C，观察复制保护期间没有退出；staging 为 542 ms，ZIP 为 539 ms。
4. 显式关闭此次 owned terminal（`cc:kill`），只允许 0 或 Windows
   `STATUS_CONTROL_C_EXIT`；两次现场均为 `-1073741510`（`0xc000013a`）。
5. 必须只有一次 `host-stop-requested` 和一次 `process-exit`，
   `stopReasonRequested/hostStopReason=cc:kill`，目标子 PID、正数 stop timestamp 和事件顺序一致；
   所有 lifecycle 行的 `lastInputAt/lastEtxAt` 为 null，没有 fallback cleanup。
6. cmd marker 与 `--version` 的退出码仍必须为 0；最终 GUI 应用退出 0，owned 剩余进程为 0。

重新使用全新 QA 目录的 staging 验收于北京时间 **21:13:42–21:13:54** 通过；最终 ZIP
验收于 **21:14:04–21:14:16** 通过。正式报告为 `.cache/release-227-stage-ui.json`、
`release-227-zip-ui.json`。这是按现有契约重新验收通过，不是放行任意 TUI 退出码；
也不声称 GUI 内 CLI 自然退出 0。gRPC helper 的自然退出 0 是独立验证项。

root 已亲自查看 `.cache/release-227-stage-ui.png` 与 `release-227-zip-ui.png`。
两张截图均为当前 owned packaged renderer 的登录界面；不代表登录后的主终端 UI。
交互 TUI 菜单证据来自真实宿主 IPC/ConPTY 数据和 lifecycle，而非截图覆盖。

## 本地六项资产快照

下表来自 `.cache/release-227-upload-manifest.json`；路径均在本轮产物根目录下。
表中 SHA256 在草稿阶段已逐项核对；正式公开后又独立回读确认十项资产的原身份保持一致。

| 文件 | 字节 | SHA256 |
| --- | ---: | --- |
| `MultiAICode-2.2.7-x64.exe` | 266110957 | `e7b7cece3bdb6e1f6cfbfea4c3f50ec5f7a605d5819e9824b4e0d10e0462b505` |
| `MultiAICode-2.2.7-x64.zip` | 360244806 | `f995450263ea908110260964f2c6f54f810c7e418f3f38de93603f5015a53b64` |
| `MultiAICode-2.2.7-x64.exe.blockmap` | 274709 | `cf0fd484f8126c1417a3b8eb19d8521baa0a56342baa212a3599d53e009e8a7b` |
| `latest.yml` | 347 | `0cfa30241db2c0c46347df9fcf7d74233fcbef5a6bd074fcf5ad2c2056da2bc0` |
| `MultiAICode-2.2.7-win32-x64.verification.json` | 26645 | `89d21446eaaa9e73f383c022eef5ed189f3584123cc831b5fe7aff2d36126200` |
| `MultiAICode-2.2.7-win32-x64-SHA256SUMS.txt` | 474 | `869f8da49f476ff0fdba97bf9ae0bf8304ddab54b0763541248ca3590a7e27c4` |

## 证明边界及远端发布结果

### Mac 协作构建恢复（产品冻结未变）

Windows 执行者在北京时间 22:35 起独立读取 GitHub job `139201252450` 的完整结束日志，
确认 run `45613044379` 的 Cargo release 已成功（102m36s），随后完成 App ad-hoc deep
签名、ABI 130 native 检查并生成 DMG/blockmap；最后 electron-builder 因未设置 `GH_TOKEN`
退出 1。这是打包后的隐式发布故障，不是 Cargo 编译失败，也不能将 job 整体标为成功。
关键日志摘录保留在 `.cache/release-227-mac-run2-excerpt.log`。

[第二轮 Actions run](https://github.com/wanhongqing123/multi-ai-code/actions/runs/45613044379)
未保留可下载 artifact；构建失败后 artifact 步骤被默认成功条件跳过。恢复命中旧 102 MB
cache，旧 `cache-hit != 'true'` 条件又跳过了保存，因此生成日志不能证明产物仍可取回。

协作者修正提交 `8fa5cb188972fbaca0ad396e65f444f3b3124eca` 仅修改 macOS workflow：
显式 `--publish never`、kernel/installer 分步、新 run/attempt 缓存 primary key，以及失败时
保留工作流 artifact。Windows 执行者通过 GitHub Commit API 独立复核了 patch；这表示
代码复核通过，**不表示第三次实际工作流已通过或 Mac DMG 已验收**。不向构建步骤提供发布 token。

正式 Mac producer 可以是 Actions 或本机独立目录，只要保持产品 `39e8eed5`/`6e09a1b5`、
Rust 1.99.1，并记录实际 profile 与包内版本/manifest，完成同等 DMG 本机验收。
Mac 既定 `CARGO_PROFILE_RELEASE_DEBUG=0` 与 Windows 源码 `line-tables-only` 分别记录。
本节仅说明协作边界，Mac 本机操作仍由 Mac 执行者验收；Windows 六项已上传资产保持不变。

本轮没有运行安装器、替换已安装或正在运行的应用，也没有使用现有用户数据或真实账户。
没有进行 OpenAI/IM 真实登录、真实消息发送、模型或付费请求；GUI 仅覆盖登录 renderer 和
真实 host IPC/ConPTY，没有渲染完整登录后主终端。TRTC native load 不代表远程屏幕共享/控制
会话已经验证。无生产源码插桩，没有以本机 Windows 结果替代 macOS 或干净 VM 验收。

下列字段记录本轮已取得的远端上传与公开发布证据：

| 远端/落库事项 | 当前状态 / 待填字段 |
| --- | --- |
| 六项 Windows 资产上传 | **PASS**；21:17:59 API 回读 id=458049005、draft=true、target=39e8eed5；6/6 uploaded；名称/字节/完整 digest 与本地重新计算一致 |
| 下载或服务端字节身份复核 | **PASS**；root 独立 API 证据见 `.cache/release-227-draft-readback.json`。Mac 协作者另回报流式读取完整安装器得到同一 SHA-256，并重算 SHA-512 核对 latest.yml；后者为协作者执行的证明，root 未重复下载整包 |
| 联合发行公开 | **PASS**；`published_at=2026-11-26T08:15:20Z`，draft=false、prerelease=false；无凭据回读确认 Latest、tag→39e、gitlink→6e、十项资产及两份校验和一致 |
| 诊断记录写入仓库 | 本文件；文档提交身份见 Git 历史。文档提交不改变发布源码与产物身份 |

若汇总报告或资产在上传前重新生成，六项资产表必须随最终 manifest 同步更新。
候选稿最初由文档子 Agent 仅通过本地报告生成；以上远端状态随后由 root 按实际上传和
独立 API 回读补充。正式构建与应用验收由 root 自己执行，不能把文档生成误作验收。

## Mac 第三轮与本机备用状态（历史阶段，最终结果见后文）

Windows 执行者独立通过 Commit API 确认 `c28b1218c41bee1a3775574e525bfe693141a286`
仅修改 artifact 名中的 run attempt 和 cache-save 条件。新 run `45636594514` 的
workflow head 精确为该提交，job `139283609463`；初次回读仍为 queued。
[第三轮 Actions run](https://github.com/wanhongqing123/multi-ai-code/actions/runs/45636594514)
随后结束为 workflow `failure` / job `cancelled`，历时 15m1s、没有执行步骤。
Windows 执行者独立用 `gh run view` 读取 annotations，GitHub 明确说明 hosted runner 多次
尝试仍未获配，并标注 macOS arm64 runner 容量不足。因此不是人为取消或产品编译失败，
也不能用 workflow patch 复核替代实际构建。原始摘要保留于
`.cache/release-227-mac-run3-runner-failure.txt`。
产品 input `39e8eed5`/`6e09a1b5` 由 Mac 协作者回报，尚未用新 job 的实际构建日志复核。

Mac 协作者另回报已准备私有 Rust 1.99.1、独立的精确 detached 产品快照与 APFS 可写缓存克隆，
采用 debug=0、3 jobs、thin LTO、4 codegen units；默认 Rust 1.96.1、现装 App 与账号保持不动。
其本机约 18 GiB 空间，将以 4 GiB 阈值停止自己的构建且不删除历史缓存。
这些为协作者回报的准备状态，不能冒称 Windows 执行者亲自在 Mac 运行了构建或验收。
当时正式 Mac producer、包内版本/manifest 和最终 DMG 验收结果仍待补齐；后文补充最终证明。

Mac 后续回报：本机备用首轮在编译前因 V8 缓存摘要不匹配被拦截，未放宽验证。
其已重新取得官方 rusty-v8 v152.2.0 的私有缓存，archive 为 24581993 字节，SHA256
`feab9ae0384bebaa99c1d000654462c9d42123d587185c7876aaeef7d76ff749`，并称通过源码
manifest pin 与 API digest 交叉核对后，用私有 TMPDIR 沿官方脚本重启。本段是 Mac 执行者回报，
Windows 执行者未下载该 arm64 archive 或执行 Mac 本机构建。

随后 Mac 协作者回报本机备用已进入 Rust 1.99.1 依赖编译，无新增错误；双方不再因该资源
故障盲目重复 dispatch。此为当时协作者报告的在途状态；后文记录了该构建后续的完整交付结果。

## 联合发布最终闭环：2026-11-26

联合发行已公开：[electron-20261125](https://github.com/wanhongqing123/multi-ai-code/releases/tag/electron-20261125)，
release ID **458049005**，**Latest**，`draft=false`、`prerelease=false`。
服务端 `published_at` 为 **2026-11-26T08:15:20Z**，即 **2026-11-26 16:15:20 Asia/Shanghai**。
联合发布由 Mac 协作者协调执行；Windows root 在发布前独立核对十项资产，发布后独立匿名回读通过。

产品冻结仍为父仓 `39e8eed575d9978535eba59a7a72a3a8a9ad1206`、
Codex `6e09a1b58f759848942e3d5b7653ab714f045ada`，宿主 `2.2.7`、
内核 `codex-cli 0.172.0+multi-ai.3d86e43b07`。后续 workflow/docs 提交没有替换该发行源码。
Windows 六项保持已验收、上传的原字节身份，没有因 Mac 收尾而重编或覆盖。

### 实际 Mac producer 与交接补正

最终 Mac 产物来自**本机独立快照，不是 Actions artifact**：macOS 27.2 arm64、Apple M3 Pro、
Xcode 27.1、Node 22.19.0、electron-builder 25.1.8；私有 Rust 1.99.1，默认 Homebrew
Rust 1.96.1 与现装 App/账户未替换。实际 release profile 为 debug=0、incremental=0、
3 jobs、thin LTO、4 codegen units，与 Windows 的 line-tables-only 分别记录。

Mac 官方入口 `npm run build:aicli` 于 **2026-11-25T15:25:19Z**
（当日 **23:25:19 CST**）退出 0，总耗时 **1843.184 秒，约 30 分 43 秒**；
协作者报告其中 Cargo 为 30 分 21 秒。该时间和 profile 已进入下载核对过的 Mac verification。
内核当晚完成后打包没有接续，协作者于次日交接承认当时尚无 DMG；此次收尾明确纠正了这段
“内核结束但未继续打包”的交接空档，不能把它描述为持续运行的编译或新的 Cargo 故障。
随后实际执行 `npm run dist:mac -- --publish never`，退出 0，再完成包验收、上传与协调发布。

### Mac 执行者的实际验收及证明边界

Mac 执行者分别在**只读挂载 DMG**和**独立复制 App**上验收通过。stage、DMG、copy 的
536 个文件/符号链接、814548775 字节内容一致；kernel/helper/main JS 身份在两环境一致。
版本/manifest 指向冻结源码；native smoke 为 Electron ABI 130、SQLite 3.49.2 与 node-pty；
六种隔离配置均按预期得到未登录状态。静态分析覆盖列明的十个 Mach-O 镜像，无未解析依赖。
otool 对 `Helper (GPU)` 路径的解释问题通过 SHA256 相同的临时副本检查解决，loader/rpath
始终锚定原 bundle，实际运行仍使用原 bundle；没有为 QA 修改产品。

两环境真实 GUI 登录界面与宿主 IPC/PTY 通过：默认请求 args 为空，宿主提供内嵌参数；
Ctrl+C 保留复制语义、没有 ETX 转发，随后显式关闭 owned terminal，观察到 exitCode 0 / signal 1。
这不是 GUI CLI 自然退出证明。独立 raw kernel PTY 显式 `--no-daemon`，看到三项登录菜单后
Ctrl+C 退出 0，是另一项实际测试。gRPC 在同一 helper 验证两个 session、V8 `42`、状态隔离
及关闭第一 session 后第二个继续执行；HTTP/2 连接关闭加 stdin EOF 后正常退出 0，未强杀。
这不单独证明 EOF 是唯一退出原因。各 watchdog 退出 0、未超时/强杀，列明的 owned PID 均退出，
bundle 可执行进程无残留。截图由 Mac 执行者亲自查看；Windows root 复核其报告，未冒称亲自在 Mac 操作。

Mac 源码检查记录类型/行宽通过、IM 507 通过/2 跳过、launch/packaging 138 通过/2 跳过。
App 为 **ad-hoc deep strict 验签通过**，没有 Developer ID、没有 Apple notarization；
unused/dead_code 及 `__eh_frame` 超过 compact-unwind 编码范围、可能影响异常处理性能的 linker
警告均保留。实际运行平台仅 macOS 27.2 arm64；Mach-O minOS 11.0 不代表已在 macOS 11 或 Intel 验收。
没有真实账户/IM 登录、模型或付费请求，也没有完整已认证主终端/真实 IM 端到端任务。

### 四项 Mac 正式资产

以下本地下载字节身份与草稿、公开 API 的 size/digest 逐项一致：

| 文件 | 字节 | SHA256 |
| --- | ---: | --- |
| `MultiAICode-2.2.7-arm64.dmg` | 302963678 | `d7704a8232a58d38086d372c1cb3aea31c4b693580ce6b659aec9b42f511485b` |
| `MultiAICode-2.2.7-arm64.dmg.blockmap` | 317338 | `e8a62ed5868adb404153c8efe988da0157cabf394299d1afa18075b6e5cd2197` |
| `MultiAICode-2.2.7-arm64-verification.json` | 94365 | `a0f8e22db295f73e9cf4ab6a3651be2ec17eedf624d6b663267cfad2ee86978f` |
| `MultiAICode-2.2.7-arm64-SHA256SUMS.txt` | 305 | `5bd4c40151b319f93944cfdd4c4c8e9a78f174bce3e69b54977f56312a8ef5f8` |

### root 独立核对与公开闭环

发布前，Windows root 在北京时间 **16:12:09–16:14:56** 的认证草稿回读确认：
十项资产名称、uploaded 状态、大小及完整 digest 全部一致；四项 Mac 文件均实际完整下载并计算
SHA256，包括 **302963678 字节的整份 DMG**；Mac sums 覆盖三项非自身资产，metadata 经独立审阅。
记录为 `.cache/release-227-combined-draft-readback.json`，其 `passed=true`。

发布后 root 于 **2026-11-26T08:15:42.997557Z–08:15:50.842155Z**
（北京时间 **16:15:42.997557–16:15:50.842155**）使用无凭据、无 cookies、无环境代理的
公开 GET 再验，`.cache/release-227-public-readback.json` 的 `ok=true`：

- release API 与 Latest API 都指向 ID 458049005、tag `electron-20261125`，且非草稿、非预发布。
- tag 直接落在冻结父提交 `39e8eed5…`；该父提交的 Codex gitlink 为 `6e09a1b5…`。
- 公开列表恰有 Windows 六项和 Mac 四项，十项均 uploaded；Windows 六项与原本地 manifest 一致。
- Windows 与 Mac 两份 SHA256SUMS 均匿名下载 HTTP 200，摘要与公开资产一致，所有非自身条目覆盖匹配。
- 公开 Release 页面 HTTP 200。匿名阶段没有声称再次下载全部十个大文件；整份 Mac DMG 的独立下载
  已在发布前完成，匿名阶段核对公开元数据及两份 sums。

Mac 执行证明位于本机 `.cache/release-227-mac-crosscheck-20261126/` 下载的 verification；
`.cache/` 证据不随仓库分发，公开 verification 与 sums 已作为 Release 资产提供。

详见 [Mac 执行者记录](multi-ai-code-227-macos-release-20261125.md) 与
[最终 Release 说明](../releases/multi-ai-code-2.2.7-20261125.md)。
