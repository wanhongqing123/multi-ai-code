# MaiChat 登录密码工具与 PDF 密码保护（2026-10-29）

## 需求与边界

- 用户要求增加修改 MaiChat 登录密码的 Agent 工具，并为现有 PDF 生成工具增加设置密码功能。普通代码入口与模型工具必须复用实现；每次改动同步维护清楚的注释。
- 登录密码工具 `maichat_change_password` 只接受 `open/status` 与请求 UUID。密码在本机安全控件输入，不作为工具参数或结果。`opening/awaiting_user/submitting` 都不代表修改成功，仅服务端严格布尔 `password_set=true` 才能报告 `changed`。绑定原账号和登录代次，拒绝把旧表单提交给新登录账号。
- 服务端已有 `POST /v1/auth/password/set`：最近10分钟登录，15–128 Unicode码点、UTF-8至多512字节；成功后撤销其他设备登录，保留当前登录。此次没有修改生产账号密码，也没有新增服务端代理。
- PDF 开关放在现有 `generate_pdf` 中，保护密码走本机私有输入回调。共用核心保留现有三端排版，使用进程内 qpdf；加密完成前不得发布明文最终文件，失败/取消保留原文件并清理本次临时文件。PDF实现与最终验证结果在后文补齐。

## 已执行的登录密码验证

- Mac27.2/Qt5.15.18，`build-release-v2.2.31` 的 `maichat`、`login_dialog_test`、`maichat_host_tools_test`、`change_password_tool_test`、`desktop_password_change_test` 编译成功。首次CTest公共工具/Host/鉴权3项通过；完整鉴权套件42.94秒，含新增加的严格服务端确认、近期登录和旧登录回复拒绝用例。
- 新原生表单测试首次失败：cocoa平台、父窗口visible且isActiveWindow为真，但 `QApplication::applicationState` 实际为Inactive，因此产品按前台要求拒绝展示。日志 `/private/tmp/maichat-password-desktop-dialog-diagnostic.log`。修正只在测试入口用公开AppKit激活独立测试进程，并等待真实ApplicationActive；未放宽产品前台门禁。最终结果后续补齐。
- 补充直接观测后确定环境阻碍：最小Qt/Cocoa进程进入真实exec后，NSApp仍未激活，前台PID855实际是系统loginwindow；`ioreg Root` 的当前console session明确 `CGSSessionScreenIsLocked=true`、onConsole与loginDone均true。此前仅查看窗口级active不足以判断App前台。已停止锁屏环境下的激活与重复测试，未停止/替换现装Mac App。桌面安全表单视觉验收暂缺，不能把3项非前台测试通过写为原生UI通过；解锁后仅跑该定向用例。
- iOS模拟器 iPhone17Pro/iOS26.5，正常Root与普通bootstrap，独立内存账号端口18390/18391/18389/18490。`AIAssistantUITests/testPasswordToolUsesSecureFormAndConfirmsServerChange` **1/1通过、零跳过**，结果 `/private/tmp/maichat-password-ios-secure-form-fixed.xcresult`：关闭表单→canceled；再次打开→双安全字段输入虚构密码→实际POST→changed；工具输出不含密码；终止App后用新密码重新登录服务端200。
- 该iOS验证没有用共享18190账号或生产账号。假IM凭据导致普通Root出现IM70402错误是此内存fixture的已知边界，不是密码接口失败。首次表单截图位于sheet进场中，不能凭它宣称完整视觉验收；后续稳定截图继续补充。
- Android首次默认GraalVM17构建停在JDK image transform，换本机OpenJDK17后隔离Debug APK编译成功。开启显式本地鉴权时，普通JVM测试的 `Build.FINGERPRINT=null` 导致AuthClient静态初始化NPE及8项连锁失败；补null门禁，未把JVM识别为设备或改变真机选择。另将JSONObject的字符串`"true"`拒绝为修改成功，只接受实际Boolean.TRUE。
- Android定向 `AuthTerminalErrorTest`、`RealtimeAsrTicketTest`、`AccountPasswordRequestsTest` **13/13通过，零跳过**。日志 `/private/tmp/maichat-password-android-unit-final.log`。首轮其余156项已通过，不将分轮结果称为一次全套运行。Android正常页面、真机安装和PDF仍待后续对应产物验证。

## 产物身份

- 此阶段以 `edcaece91462031db001a33336365bf048992f99` 为基线开发；云盘静默删除和注释约定已单独提交 `75a7c2b1f`。
- 当前手机仍为已核对的2.2.4(137)，上述密码功能尚未随137安装。最终源码提交、签名产物、装机版本与启动回读必须另记，不把源码或模拟器通过当作已更新手机。

## PDF 共用核心与跨端构建

- 固定离线源码 qpdf12.2.0，Apache-2.0许可与NOTICE随包；来源、SHA-256、C++17选择及后续上游硬化未纳入的边界见 `MaiAgent/third_party/qpdf/MAI_UPSTREAM.md`。复用已有OpenSSL、带前缀zlib和OpenCV JPEG，独立核心提供同源JPEG后备。工具只接受 `password_protected: true`，没有密码参数；原生密码回调未注册为模型工具。
- 普通C++入口 `maiEncryptPdfFile` 使用独立QPDFJob、私有日志器和排他临时文件，验证AES-256/R6、密码确实必需、页数未变后才原子发布。保留现有Qt/UIKit/Android排版，不调用外部qpdf命令。普通移动端C ABI/JNI通过独立UTF-8字节传密码，输出仅路径/元数据。
- 隔离普通核心与generate_pdf工具契约通过，保留正确/错误/空密码、Unicode密码、既有文件保护、取消后清理与原地原子替换证据。独立Poppler拒绝空/错误密码，正确密码显示AES-256并能提取原文本。日志 `/private/tmp/maichat-qpdf-probe.yOu3Lk/`。
- Mac产品主程序及新增桌面PDF私有输入目标编译成功；许可实际存在于 `.app/Contents/Resources/ThirdPartyLicenses/`。桌面PDF的UI线程拒绝和预取消契约通过，日志 `/private/tmp/maichat-pdf-native-contract.log`；前台输入视觉仍受锁屏限制，详见同目录 `maichat-desktop-native-password-forms-20261029.md`。Windows打包脚本另补许可从构建目录进入staging并核对SHA，未运行Windows构建/打包，Mac无PowerShell未执行该脚本。
- 首次iOS配置失败，qpdf上游CLI的install规则要求BUNDLE目的地；Android AGP也误编上游test_driver等测试目标。嵌入模式改为仅配置include/libqpdf，跳过CLI、测试、示例、安装规则。真实iOS SDK隔离配置及目标列表通过；随后iOS完整模拟器编译通过，Android两个ABI隔离APK构建通过。没有修改平台bundling或加密源码来绕开错误。
- 纯MaiAgent独立cache运行8套基础核心及2套PDF，共10项；首轮9/10通过。唯一旧用例 `test_session_workspace_tool_effective_next_turn` 在isBusy登记后立即改目录，worker此时未必已读取本轮目录。生产相关调用链本轮无改动；用例补等待真实模型请求到达再验证“已发出请求不变、下轮更新”，保留原断言，不扩大到新生产行为。单独重编MaiCoreTests再跑1/1通过，日志 `/private/tmp/maichat-pdf-core-snapshot-ctest.log`；其余9项含两个PDF均见 `/private/tmp/maichat-pdf-core-ctest.log`。不把分轮结果伪称一次全绿。
- iOS138首轮完整UI两项中登录改密通过；PDF锁定、错密码、原文读取均通过，唯一断言失败是把原UIKit排版的实际2页写死为1页。验收改为先读真实源PDF页数，再比较加密后页数保持一致，保留原文和密码断言；未为通过断言而修改原排版器。后续定向结果与新包身份继续补齐。
- 最终iOS138的PDF定向用例 **1/1通过、零跳过**：`/private/tmp/maichat-ios138-pdf-final.xcresult`。正常Root/创作中心进入私有安全表单，先取消且无最终输出，再双安全字段输入→UIKit原排版→共用qpdf核心加密→PDFKit独立验证空/错密码锁定、正确密码能读原文、页数与真实源一致。录像 `/private/tmp/maichat-ios138-pdf-final.mp4`；执行者查看表单和返回创作中心截图。安全输入截图不展示输入内容，未将其当作明文可见证明；假IM70402仍为fixture边界。
- 审查补齐Android UI调度超时、Activity退出/show失败时的精确request取消；iOS真实认证成功adopt推进独立登录代际，同账号重新认证也使旧表单失效，后台token refresh不推进、不重建历史/草稿。Android最终隔离APK增量构建成功，SHA-256 `a094c7873ef732fd6fd83b5bb336a833beafa879c25c35058f9e663282cc97a7`，2.2.5(237)、两个ABI与qpdf许可均在包。Android普通页面验收随后另记。
- 2026-10-29 18:54 CST左右再次只读确认：iPhone14ProMax的CoreDevice状态仍为unavailable，Mac console session仍锁屏。已请求连接/解锁，当前不能把新138标记为已安装或完成真机视觉验收。
- 首份138 generic Debug签名构建从 `4a328b6f0db5bbc23ccbd55fbac6c0943687f0cf` 成功，深度签名核验与qpdf许可检查通过。实际安装尝试CoreDevice4016失败，`...install.json`及日志 `/private/tmp/maichat-ios138-password-pdf-install.log` 记录设备无法提供连接/电源assertion；没有替换手机上的137。
- 查看完整模拟器录像的18–42秒取帧后，发现两个安全页的大标题在原半透明导航背景下持续过淡。仅将两表单改用原生行内标题，不改密码处理、其他页面或全局导航外观。PDF同一完整用例 `/private/tmp/maichat-ios138-pdf-inline-title.xcresult` **1/1通过、零跳过**；执行者亲看 `...inline-attachments/026E1889-6160-4517-A3E3-94DDF98F190C.png` 中标题已为清晰黑色，取消/确认与安全输入继续可操作。最终增量签名包与后续Android实页结果另补。

## 签名候选与安装边界

- 产品提交 `52b0a63be52c21456b7fd58ef439093e88f59183` 已推origin/main并独立回读。iOS **2.2.4(138)** generic Debug构建退出0，`codesign --verify --deep --strict` 通过。构建日志 `/private/tmp/maichat-ios138-password-pdf-final-device-build.log`；清单 `/private/tmp/maichat-ios138-password-pdf-final-candidate.json`。
- App位置 `/Users/hongqingwan/Library/Developer/Xcode/DerivedData/MaiChat-dpmznbhuepzeitfmwzvmmzmcdeql/Build/Products/Debug-iphoneos/MaiChat.app`。主文件91,856B，SHA-256 `3f409f1c04c939c9dee63fbf8dcd8ee646bbfe93567a5dd595e184a8d8bc9209`；调试dylib89,571,072B，SHA-256 `8b6937e6ccc5632c338f286a559fd8cad142a9ad17a96910b78927444f8eebdd`；QPDF LICENSE/NOTICE实际在App资源目录且有独立清单哈希。
- 19:04 CST最终设备回读仍为同一iPhone14ProMax `unavailable`。没有再次循环安装或宣称138已上手机；已授权的覆盖安装待连接恢复后继续。Mac当前原生表单仍因console锁屏未验，未替换正在运行的Mac App；Windows未构建或实机验证。普通iOS/Android模拟器、纯C++和独立阅读器证据各自保留其范围，不相互替代。

## 普通移动 API 的路径回传修复

- Android普通页面验收已生成真实加密PDF，独立Poppler空/错误密码拒绝，正确虚构密码显示AES-256、1页A4并可读；但普通Java封装把结果报失败。封装原先直接比较调用方路径字符串与核心resolve后的路径；改为核对同一canonical文件并保留核心真实路径，同一正常App流程通过。首轮未保留请求和返回路径的具体两种拼写，不将某个Android别名字串写成现场事实。
- Swift普通封装存在相同的原始路径比较，真机 `/var` 与 `/private/var` 等父目录别名可能触发此类误报。两端均按规范化路径核对。现有iOS普通PDF验收增加真正的父目录symlink别名，验证核心回传实际URL、取消无产物、正确/错误密码、源与加密页数一致；`/private/tmp/maichat-ios138-pdf-canonical.xcresult` **1/1通过、零跳过**。别名与明文测试源在结束时清理，不放宽核心对末级symlink和已有输出的限制。
- Android正常登录密码页已打开并取消，未向共享18190账号提交新密码。原生PDF路径使用普通Android排版、安全输入与共用核心，未通过模型或付费请求验收。已停止补充可选路径取证字段或扩大测试，最终APK和截图证据另附。

## 交付回执：138已安装

- 路径修复产品提交 `435f74087759fc84c99b4a16121a086af8f1d50e` 已推main。最终generic Debug构建退出0、深度签名检查通过，清单 `/private/tmp/maichat-ios138-pdf-canonical-candidate.json`。主文件91,856B，SHA-256 `a77777351bc5fb72dff22df2b07d0cad6b569411a1f175f06eb2c4732afe9a9f`；调试dylib89,571,072B，SHA-256 `62760b8f525ad0ee78ef9dca9085bc57acfcd9915f1fce7a1ce1663d632c40ba`；qpdf许可实际随App。
- 用户重新连接手机后，同一iPhone14ProMax/iOS27.2已覆盖安装 **2.2.4(138)**，没有卸载或清理数据。安装回执 `/private/tmp/maichat-ios138-pdf-installed-connected.json`，实际目录 `/private/var/containers/Bundle/Application/AE25D23F-8F30-4DD4-8B7F-9184D8FC57C3/MaiChat.app/`。独立 apps 回读版本138；普通无参数启动成功、PID6547、executable与目录一致。设备自身日志 `/private/tmp/maichat-ios138-installed-runtime.log` 确认 `2026-10-29T11:25:08.555Z app_version=2.2.4 build=138 ios=27.2 launch=ae80e558 pid=6547`，更新此前“离线未安装”的历史状态。
- Android正常App验收已完成，实际安装的2.2.5(237)隔离Debug候选202,124,370B，SHA-256 `8f15973ff5944b75f77f317c29fbf611d0e39e940fba8c939e15d2f9d37cb000`。从模拟器拉回原始APK保存为 `/private/tmp/maichat-android138-password-pdf/MaiChat-2.2.5-isolated-verified.apk` 并独立核对哈希；未采用随后未安装的可选取证构建。源码已恢复与实际通过候选一致，可选字段撤去。
- 同目录 `pdf-final-proof.json` 为passed/source_pages1/page_count1/password_protectedtrue；`protected-final.pdf`、`protected-final.txt` 和final-pdfinfo系列日志证明正确虚构密码可读原文、AES-256/1页A4，空/错误密码退出1。`pdf-final.mp4`、`canonical-secure-form.png`、`canonical-result.png` 覆盖真实安全输入和正常Root完成态，执行者与root均查看关键截图。账号页打开/取消有 `account-open-cancel.mp4`、`account-secure-form.png`、`account-canceled-settled.png` 和secure_fields=0的AX证据；取消PDF记录output_exists=false。
- 验证边界：手机实际安装与启动已确认，真实账号改密和真实用户文件操作未执行；功能流程使用隔离模拟器账号/文件验证。Mac原生窗口仍受锁屏限制，Windows未实机验证。本轮密码能力交付完成；用户随后新增的当前Session改名工具将单独记录并随后续包交付，不把它误算进138。
