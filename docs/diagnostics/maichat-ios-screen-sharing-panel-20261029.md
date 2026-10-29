# iOS 系统共享面板与 MaiChat 当前页面截图

## 现场与复现

- 日期：2026-10-28 至 2026-10-29（Asia/Shanghai）。设备为连接的 iPhone 14 Pro Max，iOS 27.2；开发 SDK 为 iPhoneOS 27.1。开始排查时工作树基线为 `703b616caa54e57f35f068c050b805349f836bf7`，云盘改名核心随后提交为 `f3cf40db5c1fcea4dc82e83194bf61c404c3ea33`。
- 用户要截取 MaiChat 云盘页面。工具先启动系统整屏共享，返回 `sharing: true`，随后抓帧却报告“这台设备当前不允许屏幕共享”。用户描述有界面把操作挡住。
- 用户后续补图纠正：MaiChat 共享期间只有底部蓝色横条，包含“截屏”和“停止共享”；会话仍可滚动。不能把系统面板误认成 MaiChat 自身全屏遮挡。
- 真机复现从正常 App 的创作中心进入“共享屏幕”，在系统选择器点“共享整个屏幕”，不调用测试框架的 App activate 来绕过转场。stream 已启动、App 状态为 inactive，但前台仍为 SpringBoard 的“屏幕共享 / MaiChat 00:01 / 停止共享”面板。此时 App 的 AX 层虽报告云盘按钮可点击，实际点击没有进入云盘。该复现点击的是底层 App 按钮，不能据此证明点击系统面板外部无效。
- 已亲看真实系统截图 `/private/tmp/maichat-ios27-share-own-repro-live-att/745C494B-7DFE-48B1-AAF2-947248A97B55.png`。日志 `/private/tmp/maichat-sc-physical-logs/maichat-ios-20261028.log` 中对应 `picker-selected`、`stream-started`、`frame-request` 记录包含 picker、stream、App 状态及错误域/码，不包含屏幕内容。

## 根因证据与证明范围

1. 原 `AIMobileScreenCapture` 只缓存 MaiChat 退到后台后其他 App 的最近一帧。它本来就不能用该缓存截取当前 MaiChat 云盘页。
2. `latestFrame` 在 stream 不存在时使用 `.unavailable`，与硬件/系统不允许共享共用同一句错误，掩盖了“共享已停止/尚未开始”和“尚无其他 App 最近帧”的区别。
3. UIKit 的公开 scene activation 实验返回 UISceneErrorDomain 0（设备不支持多 scene），未收起系统共享面板；单独令 picker inactive 也没有收起面板。对应日志为 `/private/tmp/maichat-sc-scene-logs/maichat-ios-20261029.log`、`/private/tmp/maichat-sc-deactivate-logs/maichat-ios-20261029.log`。两个实验分支均已删除，未使用私有 API、自身 URL 唤起或多窗口配置绕过。
4. 对照已安装 SDK 和 Apple 官方 [iOS 屏幕捕获说明](https://developer.apple.com/documentation/screencapturekit/capturing-screen-content-on-ios)、[UIKit 后台执行说明](https://developer.apple.com/documentation/uikit/about-the-background-execution-sequence)，本轮未找到可靠的公开接口实现“点录制后自动模拟 Home”或主动收起该系统面板。SDK 没有 App 端 dismiss API，不代表用户无法正常收起系统面板；前面的实验不能扩大为这个结论。

## 用户补证后的更正

- 2026-10-29 00:53 左右，用户提供开始前与共享计时 00:05 的系统面板截图，并亲自确认：**共享开始后，点击面板外的灰色空白区域即可收起面板，随后可以继续操作手机。** 原图分别为 IM 附件 `144115250159782623-1793206370-92232580.png`、`144115250159782623-1793206425-92232583.png`。
- 因此本轮不应按“MaiChat 自己的全屏遮挡”或“系统界面无法退出的死锁”修复。用户要收起的是系统面板并保留共享，不是要求把整个 MaiChat App 退到后台。此前对此含义有过误解，现按用户现场证据更正。
- 最小产品改动是在 start 工具描述中要求模型提前说明“开始共享后点击灰色空白处”，在宿主成功结果的 next_step 中给出同一步骤并提醒不要点停止共享。没有可用帧时等待用户访问目标页面，不反复重开选择器。没有增加新的确认弹窗或更改系统面板。
- 这一收起方式来自用户的真实现场验证；开发者在暂停手机自动操作后没有再次上机验证“收起后外部 App 抓帧”的完整链路，不能写成开发者已完成该真机验收。

## 修复和调用方式

- `mobile_capture_screen` 在 iOS 增加 `target: current_app | last_external` 与 `delay_ms: 0...30000`。省略 target 保持原 `last_external` 行为；Android 继续使用原空参数契约。
- 截 MaiChat 页面可调用 `{"target":"current_app","delay_ms":5000}`：给用户切到云盘的时间，到点以公开 UIKit renderer 保存当前前台正常窗口。此路径不启动整屏共享、不呈现系统选择器，不读写其他 App 的缓存。结果明确 `source=current_app`、`whole_device=false`，不能当作整台设备或其他 App 的截图。
- 普通 Swift API 为 `AIMobileScreenCapture.capture(to:workspace:target:delayMilliseconds:)`；UI 和宿主工具复用该实现。只接受工作区 `screenshots` 下新建的 `mobile_screen_*.png`，拒绝路径逃逸、目录符号链接及覆盖；PNG 有大小限制并排除系统备份。
- 前台不活跃或系统面板未收起时给出可操作错误。共享没有开启/已经停止用 `not_configured`；没有有效外部缓存用 `not_found`，不再一律报告设备不允许共享。
- 外部帧仅在 App 真正 background 时接受，inactive 系统面板不会被当作外部 App 缓存。回 MaiChat 不覆盖已有外部帧。蓝色状态条保留原样。
- Agent 调用链是 C++ 同步宿主桥；Swift Task 的取消不会自动对应 Agent turn 的取消。因此 Agent 延迟在 `MaiMobileHostTools.cpp` 按 25ms 间隔检查 `context.isCanceled`，取消后不再进入宿主截图。宿主接收 `delay_ms=0`，避免双重等待；普通 Swift 调用仍用可取消的 `Task.sleep`。宿主返回期间若 turn 已取消，只清理本次工具拥有的 PNG。

## 本地验收

- iOS 模拟器 `testCurrentAppScreenshotUsesWindowWithoutScreenSharing` 1/1 PASS、零跳过；结果 `/private/tmp/maichat-current-app-capture-final.xcresult`。普通 API 和 Swift Agent 宿主分别生成有效 PNG；验证参数拒绝、重复文件/越界路径拒绝、普通 API 取消不留 PNG，且共享前后均为 false。
- 已亲看宿主实际输出 `/private/tmp/maichat-current-app-host.png`（1206×2622），包含正常可见测试画面“MaiChat 当前页面截图 / CURRENT APP ONLY”，不是空白图或测试框架截图替代物。普通 API、宿主和一次性验收 hook 各自输出独立 PNG。
- DEBUG 的 `MAICHAT_CURRENT_APP_CAPTURE_SMOKE=YES` 仅供约定的集中真机验收：正常 App 切至云盘后一次性等待 750ms，调用同一普通 API，写入 `Library/Application Support/MaiChat/ScreenCaptureSmoke/<UUID>/result.json` 和独立 PNG。默认关闭，不改会话、不开始共享。模拟器已验证 JSON 与 PNG；实际云盘内容须在真机候选上另行核对。
- C++ `MaiMobileAgentTests --capture-contract-only` 在 iOS 模拟器 native 与 Android emulator native 各一次 exit 0。真实 context 在 5000ms 延迟的约 50ms 处取消后，宿主调用与 PNG 均为零；正常延迟 75ms 后进入宿主且 `delay_ms=0`；宿主返回时取消只清理自己生成的文件。两个平台的参数 schema 和 Android 原路径也通过。日志 `/private/tmp/maichat-screen-capture-native-contract-{ios,android}.log`。该层只用 PNG signature 验证协议，不代替 Swift 输出图片的解码/画面证据。iOS 独立测试目标使用本机链接参数补入既有 FFmpeg SecureTransport 所需 Security framework，没有修改产品 CMake；完整 App 原本已链接该 framework。
- 最终产品源码提交 `a12184bc541d6fd7983005e3a3f2a213d6932d87` 已推送并从 origin/main 独立回读一致，包含用户补证后的共享指引。该提交的本地 `generic/platform=iOS` Debug `build-for-testing` exit 0，`codesign --verify --deep --strict` 通过，仅构建而未连接执行真机测试。候选为 `MaiChat-dpmznbhuepzeitfmwzvmmzmcdeql/Build/Products/Debug-iphoneos/MaiChat.app`，版本 2.2.4 (131)，`MaiChat.debug.dylib` SHA-256 `3080572127f5a17062f696a49569283220018a7ad0e4405e3753b1c6a7bc145f`。日志 `/private/tmp/maichat-ios-cloud-capture-final-build.log`，清单 `/private/tmp/maichat-ios-cloud-capture-final-candidate.json`。这份最终候选取代前一 `799852f6...ddb983` 本地候选，不代表手机已更新。
- 同轮云盘目录模拟器 2/2 PASS、零跳过，证据在 [云盘目录记录](maichat-cloud-drive-folders-and-ios-gallery-20261028.md)。本轮改名服务部署与宿主验收在 [改名记录](maichat-cloud-drive-file-rename-20261029.md)。

## 手机操作暂停与剩余边界

- 用户指出来回测试影响手机使用后，已停止真机 UI 自动化、安装和启动；后续只做本地验证及签名候选准备。未把模拟器通过写成最终合并 App 已装机。
- 下一次手机操作应在双方约定的一个集中窗口内完成一次安装、正常云盘目录浏览和一次 current_app PNG 回读；不继续系统面板探索实验，不移动、改名或删除用户真实云盘文件。
- 已准备 opt-in 的 `CloudDriveDeviceReadOnlySmokeUITests/testInstalledCloudFolderReadOnlyRoundTrip`，通过上述本地编译但尚未运行。它只启动正常 App 一次，进入约定的已有目录、等待首项图片解码/视频首帧、关闭预览并返回根目录；不操作账号或云盘数据。不能将“用例已编译”写成“真机已验收”。
- 当前页面截图无法包含其他 App、系统界面或保证 GPU 视频画面；其他 App 的整屏共享仍由系统授权与用户切换控制。用户已确认手动点灰色空白收起面板，App 主动收起系统面板或自动退到后台本轮未实现。

## 用户授权安装后的实际结果（2026-10-29）

- 用户随后明确“安装到手机”，恢复本次集中安装窗口。安装前重新核对清单中主 Mach-O 与 dylib 的 SHA-256，以及签名、版本和源码差异；候选仍为上文 `a12184bc5` 的产品代码，后续 `52167d2e6` 仅更新本记录。
- 首次使用预构建的单个只读用例交给 Xcode 安装并验收。runner 在进入产品用例前因 `Timed out while enabling automation mode` 初始化失败，xcodebuild exit 65。结果 `/private/tmp/maichat-ios-final-cloud-readonly-20261029.xcresult` 的 failure 属于 `MaiChatUITests-Runner (4130)`；没有执行文件夹、预览或截图的业务断言。不将基础设施失败写成产品回归，也不把它算作验收通过；没有重跑 UI 测试。
- 随后直接以 `devicectl device install app` 覆盖安装已核对的同一候选，exit 0；安装回执 `/private/tmp/maichat-ios-final-direct-install.json`。未调用卸载或清理 App 数据。普通启动 exit 0、无测试参数，启动回执 `/private/tmp/maichat-ios-final-launch.json`。
- 最终设备回读 Bundle 为 `com.kongshang.maichat`、版本 2.2.4 (131)，安装路径 `/private/var/containers/Bundle/Application/5B68B718-C3F2-4D5F-896A-541482DB78AC/MaiChat.app/`。实际启动 PID **4137** 的 executable 位于同一路径。App 自身日志 `2026-10-29T00:55:02.538Z` 记录 `app_version=2.2.4 build=131 ios=27.2 pid=4137`，随后的 `app-active` 为 `00:55:02.620Z`；证据 `/private/tmp/maichat-ios-final-installed-readback.json`、`/private/tmp/maichat-ios-final-runtime-20261029.log`。
- 至此“安装并启动修正版”已完成。原生 UI 自动化权限仍阻止本轮页面验收，因此没有宣称真实云盘目录浏览、current_app PNG 或外部共享完整链路在该真机候选上通过。此前模拟器和 native 契约证据继续有效，真机未验证范围保留。
