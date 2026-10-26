# iOS 顶部错误条遮住创作中心操作（2026-10-24）

- 平台与现场：iPhone Air iOS 26.4 模拟器；本机假账号与假腾讯 IM 返回 70402。基线提交 `8ac971d3104ae447f965e8fb6ea0c0d479e4f248`，工作树含未提交的 iOS build 116 视频圈首版。
- 复现：正常 MaiChat App 登录本机测试账号，进入创作中心。红色“IM 操作失败 (70402)”提示出现时，点击右上角视频圈。第一次点击只关闭红条，第二次才打开视频圈。XCTest 记录顶栏按钮 frame 为 `(308,73,44,44)`；首点后定点计数为 `button=0/open=0/route=none`，确认没有进入按钮 action。
- 根因证据：RootView 把长错误条作为全屏顶层 `.overlay(alignment:.top)` 显示，仅向下偏移 8pt。失败前截图中红色胶囊覆盖了整个 y73–117 的标题栏；失败后截图中胶囊消失，标题栏露出。证据在 `/tmp/mai-video-circle-v27-shots/D53EB5DE-E437-45BA-BBDF-756BA0424BC2.png` 和 `/tmp/mai-video-circle-v27-shots/F697AA96-93A7-408F-91BF-2EE9FCD42A05.png`。临时关闭会话抽屉 GeometryReader、全页抽屉拖拽手势以及无播放器时的播放器 overlay 均未改变 `button=0`，排除了这些层。
- 修复：RootView 将错误条放在 54pt 顶栏下方（顶部偏移 64pt），最多显示两行，完整错误保留为无障碍标签；仍可点击错误条关闭。无播放器时不再创建全屏播放器 GeometryReader。
- 回归：同一红条首点用例由失败变为通过。iOS 模拟器 build 116 正常 App 定向测试 `testVideoCircleNormalAppOpensPlaysAndReturns` 1/1 通过（20.5 秒），首点进入、播放、关闭、退出、二次进入均成功；双账号完整本机流程 1/1 通过（57.5 秒）。结果包分别为 `/tmp/mai-video-circle-normal-v29-build116.xcresult`、`/tmp/mai-video-circle-full-v30-build116.xcresult`；后者截图在 `/tmp/mai-video-circle-full-v30-shots/`。
- 真机安装：签名包 `/tmp/mai-video-circle-ios-release116/Build/Products/Release-iphoneos/MaiChat.app` 的 Bundle ID 为 `com.kongshang.maichat`，版本 2.2.3(116)，Mach-O SHA-256 为 `9600b3fae94c7efa4c595d8de4cf7ba3069149edd850a1d4920fbc7d8899ed6d`，`codesign --verify --deep --strict` 通过。2026-10-24 04:16（中国时间）通过 CoreDevice 覆盖安装到 iPhone 14 Pro Max，设备安装列表回读 2.2.3(116)，启动后进程 PID 10070 的执行路径属于新装 App。
- 边界：假 IM 的 70402 来自本机测试凭据；未验证真实腾讯 IM 的错误文案。真机屏幕画面未捕获，不能把模拟器视频圈 UI 验收称为真机体验验收。视频圈线上功能开关仍关闭，修复尚无源码提交。
