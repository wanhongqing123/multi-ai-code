# 2026-11-12 Codex 同步后的原生回归

## 复现和预期

在固定官方上游 `e58bf8f5233b51e7cd1679d947e3028c1ed91b06` 的定制子仓上，
执行 CLI/TUI/config 原生回归，并在 Windows 以 8 MiB 栈预算执行 IM、权限和侧边会话用例。
预期保留 IM 自动返回、交互式侧边会话、无沙箱选项和重连行为，且真实版号能正确展示。

相关侧边会话用例为 `remote_im_side_creation_preserves_auto_return_before_thread_started`
与 `handle_start_side_seeds_navigation_before_thread_started`；重连用例为
`reconnect_daemon_command_center_after_socket_replacement_without_a_conversation`。

环境为 macOS arm64 和 Windows 11 x64；宿主候选 `89dd9f115`，
子仓候选 `daa8d7c3e5ddcc08d5d47d4061975249be461c89`。
这是源码测试构建，尚非已发布安装包或用户正在运行的版本。

## 现场事实和定位

- macOS 首次执行 6,906 项，6,865 通过、41 失败，另有 5 项跳过。
- 40 个失败用例首先停在快照断言：上游 `0.0.0` 被真实版号
  `0.167.1+multi-ai.e58bf8f523` 替换后产生换行、对齐变化；还包含保留的
  `Continue without a sandbox` 选项及上游页脚变化。继续执行后又暴露同组后续断言，
  合计人工审查和更新 44 个快照文件，没有过滤版本内容。
- PTY 启动用例在相同二进制、相同超时下，以 `TERM=dumb` 失败、以
  `xterm-256color` 通过。该测试启动的终端子进程需要明确的终端能力，
  不应继承外层无交互测试环境的 `dumb`。
- Windows IM 侧边会话测试在 8 MiB 下以 `0xc00000fd` 栈溢出，两次尝试一致；
  32 MiB 对照通过。以 `FnOnce -> Future` 类型推导测量得到 helper future 为
  53,192 B、直接 await 的 wrapper 为 53,200 B、使用项目已有 boxing 工厂后为 16 B。
  测量没有先构造大型 future；临时插桩已恢复。
- macOS 重连用例在快照通过后同样出现栈溢出，崩溃记录经过 `Config::clone`、
  `resume_thread`、`thread_attachment::prepare`、会话选择及事件分发，
  未见无限递归证据。32 MiB 对照通过。

## 修正和证明范围

子仓 `ac71f246052620472c2693d56a79ad5a1304f3d5`：

- 两个侧边会话 wrapper 使用已有的非内联 `boxed_future!` 工厂，
  防止 helper 的完整临时值和 poll frame 留在 caller 栈上。
- 重连测试只在大型事件处理入口采用同一工厂；没有修改生产逻辑或正式栈预算。
- PTY 测试仅设置其子进程 `TERM=xterm-256color`；没有增加超时。
- 版本与菜单快照按真实产品行为校准，保留无沙箱选项的后续操作断言。

macOS 两个侧边会话用例及重连用例在原 8 MiB 下全部通过；
首次失败的其余项目亦逐项复验通过，最后一个状态页用例在关闭快照更新的标准模式下通过。
原始全量执行和后续定向复验共同覆盖同一组 6,906 项，不能描述成首次全量一次通过。

Windows 在最小 boxing 对照中已证明 IM 用例由栈溢出变为通过。
额外加入的交互式用例在图片拖拽释放后观察到分栏坐标 199，而预期为 197；
拖拽预览 `[197, 197, 199]` 已通过。加入日志的对照中，鼠标释放后 drag 清除、
宽度百分比为 34、配置文件正确追加设置且 mtime 增加，没有宽度保存错误。
移除释放前后的读取和打印、仅在保存失败分支留临时诊断后，连续 3 次通过，
失败诊断均未命中。测后已恢复所有插桩，未放宽断言或改变生产行为。
这个结果只证明这些复验通过，不能反推此前一次差异的根因或宣称产品已修。

正式优化构建、安装包内版本、原生模块加载和隔离启动验收另记于发布记录；
以上源码测试不证明当前用户进程已更新。
