# 当前会话改名工具与显式命名保护（2026-10-29）

## 需求与代码入口

- 新模型工具 `session_rename({"title":"..."})` 只修改发起调用的当前根会话；不接受目标会话 ID，不创建会话、不切换界面，不修改工作区、模型或聊天历史。
- 工具在 `MaiSessionTools.cpp` 的 `registerMaiSessionTools` 中无条件注册，移动端关闭本地工作区切换时仍可使用。
- 普通入口仍为 `MaiAgent::submit(MaiUpdateSession)`。工具使用该入口完成持久化与 `SessionUpdated` 广播，并重新读取存储后返回真实的 `session_id`、`title`、`changed`。
- 改名遵循现有普通写操作审批；没有付费标记或额外逐次审批，完全访问模式下不要求新增确认。
- 名称上限与既有 `services/auth/session-sync-service.js` 保持一致：120 个 Unicode 标量、480 个 UTF-8 字节；拒绝无效 UTF-8、空/全 Unicode 空白、C0/C1 控制字符与换行，不静默截断或 trim。普通更新的空标题仍表示“不改”，工具的空标题则被拒绝。

## 已复现的自动命名覆盖

源码基点为 `c37c8474997c8fcc08863fb435231803d3c82643` 加本轮工作树修改。

1. 创建默认标题的会话，使用真实 `MaiFakeModelClient` 工具调用链执行 `session_rename`，新名称为有效的 `New session`。
2. 等待本轮结束，再继续发送一轮消息。
3. 预期：两轮后标题均保持用户指定名称；实际修复前两处断言均失败，名称被首条用户消息覆盖。证据为 `/private/tmp/maichat-session-rename-red.log`。

根因为 `MaiTurnRunner::finish` 在工具完成后调用 `MaiSessionTitler::apply`；旧的 `MaiSession::isUntitled` 只比较空字符串或占位标题字面量，无法区分用户有意使用的同名标题。仅在本轮设置内存开关不能保护 App 重启后的下一轮，因此没有用临时状态或扫描工具历史规避。

## 修复与存储兼容

- 新增 `MaiSession.titleEdited`。普通 `MaiUpdateSession` 写入非空合法标题时，在同一个存储 mutation 内保存名称和显式标记；Titler 的快照和锁内复查都通过 `isUntitled` 检查该标记。
- SQLite 在现有版本迁移链追加事务性 V6，增加 `title_edited INTEGER NOT NULL DEFAULT 0`。旧表、旧会话与消息不重建，旧记录默认保留此前的自动命名资格。迁移在写锁内检查已有列，兼容已有列但版本号落后的情况。
- 新建占位会话行为保持原样。清空历史原本就会清除标题，现在同时清除显式标记，让下一轮继续正常自动命名。
- 已命名同名调用为无操作，不提交更新或重复广播。首次把未命名占位标题明确命名为同文本时，仅保存必要的显式标记，返回 `changed:false` 表示文字没有改变。
- 普通更新所有字段均无变化时不重复广播；写入失败不发布成功事件。模型结果还会回读实际标题，防止将请求值当作已持久化的事实。

## 本机验证

环境为 macOS、Clang、C++17、Release，测试缓存 `/private/tmp/maichat-pdf-core-gate`；只构建和测试，未操作手机、生产服务或现用桌面 App。

- `MaiSessionRenameTests`：真实工具调用、完全访问不新增审批、当前会话限定、拒绝额外目标 ID、拒绝子 Agent、名称校验、中英文/四字节 UTF-8 上限、同名无重复事件、事件前已持久化、其它会话与字段不变；占位同名本轮/下一轮保持；真实 V5 库升级、字段默认值、SQLite 关闭重开、普通 API 改名、清空后恢复自动命名全部通过。
- 定向 CTest：`MaiCoreTests`、`MaiSessionRenameTests`、`MaiStoreTests` 3/3 PASS；新改名用例 0.43 秒。日志 `/private/tmp/maichat-session-rename-green.log`。
- 规定前八套核心回归 8/8 PASS，70.98 秒。日志 `/private/tmp/maichat-session-rename-core-gate.log`。
- 新构建 `maiagent-console` 的 `tests/e2e/console_e2e.py` 全部通过，包括标准输入输出、会话切换、重新打开数据库与进程强杀后的历史恢复。日志 `/private/tmp/maichat-session-rename-console-e2e.log`。
- 八个新改 C++ 文件完成 clang-format，dry-run 与 scoped diff-check 均通过。

## 验证边界

以上证明共享核心与真实 SQLite/模型工具链。各端 App 接线、界面刷新、手机安装版本与 Windows 构建由本轮主执行者单独验收；此文不宣称这些平台产物已经替换或安装。修复提交号由主执行者统一提交后补充。

## App 接线与界面验证

- Desktop继续复用核心注册、普通更新、SessionUpdated和presence目录广播；没有新增同义host工具。修复原面板只在当前session事件时刷新全局侧栏的过滤：后台A改名时，侧栏也更新，但当前B页面、选择与消息保持不变。主程序和 `agent_panel_session_test` 编译成功，`anotherSessionTitleDoesNotRenameTheHeader` 定向用例通过，日志 `/private/tmp/maichat-session-rename-desktop-panel.log`。未替换现装桌面App。
- iOS/Android现有适配器将SessionUpdated标记为结构变化，随后会话快照含新标题；两端列表已有刷新链，无新增宿主动作或改名界面。跨设备目录继续原15秒presence周期，本轮未把逐token事件改成频繁广播。主页面标题固定为“创作中心”，验证对象是会话列表。
- iOS正常Root新增唯一显式本地模型marker的测试，用实际Responses工具调用执行改名，未直接改测试服务的会话名。首次因独立Auth/Model使用18390/18389，而WSS仍硬编码18191，手机连接/presence/model计数均0，尚未执行改名。仅模拟器本地测试配置补齐模型与WSS端口变量、open/save识别一致，保留TLS证书校验；生产设备URL/模型配置未改，未重启共享18190。
- 修正后 `/private/tmp/maichat-ios139-session-rename-socket.xcresult` **1/1通过、零跳过**：通过正常UI选择完全访问，工具回读当前ID和新标题；侧栏同ID立即显示新名称；未发送草稿在改名与切换会话后保留；相同测试数据重启后原ID、标题、历史仍在。草稿原本只驻内存，不宣称新增跨重启草稿能力。正常Root测试模式重启会新建默认会话，不以重启后的默认选中对象作为改名失败依据。
- 执行者亲看 `23E6CE16-AE91-4116-A720-BC8CAF87E665.png` 当前会话选中并显示 `Pinned rename-45d1ea5b`，以及 `8972780F-E24F-42E1-9310-35A095621DA0.png` 重启后该名仍在列表。附件位于 `/private/tmp/maichat-ios139-session-rename-socket-attachments/`；录像 `/private/tmp/maichat-ios139-session-rename-socket.mp4`。截图IM70402来自本地假IM凭据，为已知fixture边界，不代表改名失败。
- Android仅做新共享核心/SQLiteV6的完整隔离Debug编译，两个ABI实际库含 `session_rename` 与 `title_edited`，qpdf许可仍在包。APK177,560,135B，SHA-256 `7b452912a9747ea90fd95d31bb1f197498110768941f8d335eee7c5dfbcb34fa`，记录 `/private/tmp/maichat-android139-session-rename-artifact.json`；本轮未安装Android或重复先前已完成的PDF/账号UI验收。
- 新iOS候选为2.2.4(139)。签名、手机安装与启动身份在完成后追加，不能把此前已装138写成包含此工具。

## 139手机安装回执

- 产品提交 `965179b5f54158aeae452de6eb0bcd084a7098fd` 已推main，generic Debug构建退出0、深度签名核验通过。清单 `/private/tmp/maichat-ios139-session-rename-candidate.json`：主文件91,856B，SHA-256 `0893c48a227776d0feab5d8d737b12e1b21b7c18569247d362bd415ab2326446`；调试dylib89,588,640B，SHA-256 `180b161fb14688b1f8572450bd63ed1316b55c95647ab720066472f1e1bd037f`。
- 同一iPhone14ProMax/iOS27.2已覆盖安装2.2.4(139)，独立apps回读确认。安装目录 `/private/var/containers/Bundle/Application/0EC4116E-A58E-4EA6-97F9-1F7C3D49C746/MaiChat.app/`，普通无参数启动PID6609，executable与安装目录一致。回执 `/private/tmp/maichat-ios139-session-rename-install.json`、`...installed-app.json`、`...launch.json`。
- 设备自身日志 `/private/tmp/maichat-ios139-session-rename-runtime.log` 记录 `2026-10-29T12:03:06.773Z app_version=2.2.4 build=139 ios=27.2 launch=e361e89b pid=6609`。执行者查看整个125秒模拟器录像的2秒取帧，覆盖权限设置、保留草稿、工具改名、切换、重启恢复；没有将该取帧范围称为真机连续UI验收。未修改真实用户会话名，真实手机只安装和正常启动。
