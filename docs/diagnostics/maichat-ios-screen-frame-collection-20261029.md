# iOS 屏幕共享定时留图与选择

## 需求、版本和操作

- 日期：2026-10-29（Asia/Shanghai）。功能提交 `6903bea50a3d0322bf983434a7fb66ab1d149db7`，加上真实 SDK 析构编译修正后的候选源码为 `5a03e0ca275c1c036810733e36b027a8ab7784a3`，iOS 版本 **2.2.4 (132)**。
- 用户要求开启整屏共享后固定每 2 秒留存画面，停止后从本次截图中选择需要的图片；不要求模型循环调用截图工具。开始共享仍需 iOS 27 及以上真机的系统授权，系统面板的用户操作说明沿用 [此前记录](maichat-ios-screen-sharing-panel-20261029.md)。
- 普通操作为创作中心开启共享、查看蓝条上的采样间隔和已保存张数、停止共享、在“本次截取 N 张，选择需要的图片”网格中预览或多选、加入开始共享时的原会话草稿。此时不会自动发送消息。关闭选择页保留档案，可再次打开；只有明确“清空本批”才删除该批原图。

## 实现与边界

- 共用本地核心为 `MaiChat/Network/MaiScreenFrameArchive.h/.cpp`。普通 C API `mai_screen_frame_archive_request` 支持 `begin/append/finish/list/get/discard`；Swift 的 `AIMobileScreenFrameArchive` 包装同一 API。界面和系统采集不通过大模型工具协议调用。
- 系统采集回调交给正式 Swift sampler，以独立的 2 秒定时器采样、编码并写入 PNG；MaiChat 自身 inactive 阶段跳过，并清除跨阶段的旧像素，避免保存本 App 的系统选择器/切换过渡帧。外部 App 上方的系统界面仍属于共享的整个显示器，公开帧中没有可靠的外部 App active 标记，未增加视觉猜测过滤。停止、系统中止、采集失败或容量达到上限均结束批次并保留已保存图片。没有增加模型轮询、自动重开系统选择器或常驻模型请求。
- Agent 工具 `mobile_start_screen_share`、`mobile_stop_screen_share` 使用实际会话 context；`mobile_screen_frames` 支持 `list/open/delete_batch`。列出仅返回编号、时间等元数据，不把整批图片自动发给模型；明确打开某一帧时，经 Swift 普通导出入口生成独立副本再返回图片。正在录制的批次不能通过该工具删除。
- 档案保存在工作区 `.maichat/screen-frame-archive`，按工作区及非空 owner key 隔离。owner 支持实际 Session、远端会话键和空草稿的 `draft:UUID`，不会把用户输入直接拼成档案路径。`get` 重新验证文件、PNG 与摘要，不直接信任调用方给出的路径。
- UI 开始前保存账号、工作区和原附件 target；切换会话不会将 A 的图片写入 B。空草稿发送生成实际会话后复用现有附件 target 映射。导出 await 后再次检查账号、原 target 和附件余量，失败清理本次导出的副本；单条消息继续最多 10 项附件。账号切换停止采集，旧异步任务不能写进新账号草稿。
- 被选图片复制到原工作区 `screenshots/mobile_screen_<UUID>.png`，普通选图与 Agent 打开的图片都不直接引用临时档案。清空原批不会删除这些附件副本。选择页的缩略图为懒加载和有界解码，PNG 验证与图片解码不在主线程执行，离屏释放缩略图视图持有的解码状态。
- 异常退出后磁盘批次可能仍标为 `recording`。同账号、工作区及 owner 的恢复列表会把没有活跃采集器的遗留批次 finish 为 `interrupted`，保留图片并允许后续选择或清空。没有给空草稿增加跨进程迁移流程。
- 核心限制为帧间隔至少 2000ms、每批最多 300 帧或 256 MiB、每工作区最多 512 MiB、单 PNG 不超过 20,000,000 字节。达到限制停止采集，不自动删除旧图腾空间。停止流程先改变共享状态、随后完成档案收尾；UI 等批次变成 `stopped` 后才自动展示选择页，避免提前清空仍在写入的批次。显式清空成功还会移除 manager 的已停止批次引用，防止蓝条重新出现已删批次。
- 本轮只接入 iOS 产品。Windows、Android 没有新增该产品功能。核心仅在 iOS、Network 测试或显式 `MAICHAT_SCREEN_FRAME_ARCHIVE_ENABLED=ON` 时构建；该开关默认 OFF，普通其他平台产品不因此新增 ZLIB 构建依赖。

## 本地验证证据

- scoped CTest `mai_screen_frame_archive` **1/1 PASS，2.54 秒**：普通 API 的批次重开、归属、并发、间隔/容量限制、PNG 校验和明确删除等检查。日志 `/private/tmp/maichat-screen-archive-scoped-test.log`，构建目录 `/private/tmp/maichat-screen-archive-native`。
- 最终 iOS 模拟器 `ScreenFrameArchiveUITests/testRealSamplerSelectionStaysOnOriginalDraftAndDiscardKeepsCopies` **1/1 PASS，零跳过**，设备 iPhone 17 Pro / iOS 26.5。结果 `/private/tmp/maichat-screen-frame-ui-final-20261029.xcresult`，日志 `/private/tmp/maichat-screen-frame-ui-final-20261029.log`。该测试使用真实 sampler、生成的安全 `CVPixelBuffer` 和正式 C++ 档案，不以假 metadata 替代存储；验证 2 秒采样、inactive 跳过、停止保留、图片实际解码、取消后重开、从 B 打开 A 档案仍只写 A 草稿、账号拒绝和清批后副本可读。
- 最终 UI fixture 另实际调用 Swift 宿主 `mobile_export_screen_frame`；错误 owner 返回 `not_found`。清批后再次读普通选图的两份 PNG 和宿主导出 PNG，均保留有效内容。此证据覆盖正式导出宿主，不等同于模拟器运行了 iOS 27 系统 ScreenCaptureKit。
- iOS native `MaiMobileAgentTests --capture-contract-only` 编译和运行 PASS；普通 C API append/finish 后，Agent 路径 list/open/export，再 delete_batch，验证已导出的副本仍可读。日志 `/private/tmp/maichat-screen-frame-agent-contract-build.log`、`/private/tmp/maichat-screen-frame-agent-contract.log`。首轮测试把空 PNG 的指针传成非空，违反无图片请求的普通 API 契约；fixture 修为 `nullptr` 后通过，该问题不是产品故障。
- 前一轮同一正式选图 UI 的结果为 `/private/tmp/maichat-screen-frame-ui-20261029.xcresult`，**1/1 PASS、零跳过**。已亲看 `/private/tmp/maichat-screen-frame-ui-20261029.mp4` 和导出附件：网格两列、四帧的序号/时间完整，蓝勾和加入草稿按钮清楚；大图真实解码并居中适配；关闭后重开仍有四帧；加入后画面 A=2、B=0，余量由 10 变为 8；明确清空后显示副本验证通过。67–71 秒按 6fps 检查大图打开/关闭，系统容器连续展开和收起，未见中途空白闪帧。
- 亲看证据：`/private/tmp/maichat-screen-frame-ui-20261029-attachments/D2C2E049-BF4E-4143-A7B9-6F05B6644A8F.png`（多选网格）、同目录 `91471F75-2DFE-4948-A061-2BAF8B223533.png`（解码大图）；录像取帧 `/private/tmp/maichat-screen-frame-reopened.png`、`/private/tmp/maichat-screen-frame-original-draft.png`、`/private/tmp/maichat-screen-frame-explicit-clear.png`、`/private/tmp/maichat-screen-frame-copies-survive.png`、`/private/tmp/maichat-screen-frame-preview-transition.png`。

## 真机状态与后续回执

- 此前安装的 2.2.4 (131) 真机候选，在集中只读 XCTest 初始化时被系统 UI Automation 权限阻止，未执行本轮产品断言；原始安装和权限失败回执见 [此前装机记录](maichat-ios-screen-sharing-panel-20261029.md)。不能把这次基础设施失败写为本功能通过或产品回归。
- 132 首轮真实 SDK 的 Swift 6 generic 构建发现 `AIMobileScreenCapture.swift` 的 driver `deinit` 读取四个非 Sendable 的 `NSObjectProtocol?` observer，编译失败；日志 `/private/tmp/maichat-screen-frame-ios132-build.log`。模拟器条件编译不包含该 driver，先前独立 Swift 5 typecheck 也没有覆盖这个 Swift 6 析构隔离约束。执行者按编译器提示最小改为 `isolated deinit`，在同一 MainActor 上析构和移除 observer；这是 `6903bea50` 后的修正，未改变采样或选图协议。
- 修正后 `generic/platform=iOS` 最终构建 exit 0，日志 `/private/tmp/maichat-screen-frame-ios132-final-build.log`；`codesign --verify --deep --strict` 通过，产物 Info.plist 为 2.2.4 (132)，dylib 存在 `mai_screen_frame_archive_request` 的实际链接符号。候选清单 `/private/tmp/maichat-screen-frame-ios132-candidate.json` 对应原 DerivedData `Build/Products/Debug-iphoneos/MaiChat.app`：主 Mach-O 91,856 B / SHA-256 `68ebc943f670fcb49b42b58c0f2103ea6439946ea9c839623c341963eb97b4db`，`MaiChat.debug.dylib` 86,096,608 B / SHA-256 `cfa0f36159a00d9fe0aff4b660badc8815ab1a0a6e5cefddc13a312e12aa9bfd`。这是本地候选，不代表已装机。
- 已准备默认关闭的 DEBUG 普通 App 验收 hook `MAICHAT_SCREEN_FRAME_ACCEPTANCE=YES`，不依赖此前被阻止的 XCTest UI Automation。它使用原账号、正常创作中心及普通 start/register，系统授权仍由用户本人选择；开始后采样十秒自动停止、保留本批，不发送模型、不加草稿、不删除图片。`Library/Application Support/MaiChat/ScreenFrameAcceptance/<UUID>/result.json` 写入开始/停止状态和帧元数据；回到前台后另用普通 current_app API 保存实际界面 PNG。该 hook 本轮尚未在手机执行。
- 最终候选就绪后已询问用户一次集中真机采样的方便时间；未得到回复前不安装、弹系统选择器或切换用户手机 App。真实 ScreenCaptureKit、面板收起后其他 App 采样、正常 Composer 和停止选图的真机链路仍未验证；锁屏等额外边界也未验证。
- 当前模拟器证据覆盖正式本地 sampler、档案、Swift 宿主、选图界面及会话草稿归属；不覆盖正常 Composer 蓝条在实际手机上的完整共享操作。本轮没有因记录工作再次启动手机自动化、安装、切换 App 或操作真实云盘数据。
