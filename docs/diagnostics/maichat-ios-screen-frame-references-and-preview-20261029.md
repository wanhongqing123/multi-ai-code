# iOS 截图原图引用、大图预览与全 App 半透明栏位

## 用户反馈与源码范围

- 时间：2026-10-29（Asia/Shanghai）。本轮基于 `a07627edd` 的 132 安装记录继续开发，产品提交为 `d243d6c44db77959d482138bdb694e450e956290`，iOS **2.2.4 (133)**。该候选已完成 generic 真机构建与签名核验，但尚未安装；随后用户追加全 App 半透明上下栏和云盘简化导航需求，将合并完成后统一装机，避免反复替换手机 App。
- 用户要求截图留在 Session 的本地工作区（sandbox），选中的图片直接使用原图，不再复制一份重复 PNG。本轮以原图引用替代 132 的导出副本方案；[上一轮记录](maichat-ios-screen-frame-collection-20261029.md) 中的副本行为属于历史版本，不是并行保留的另一条产品路径。这里没有引入 Codex 操作系统沙盒或新的模型请求服务。
- 用户同时反馈截图大图四周有黑框，怀疑使用 ffplay，指出 IM 和 Session 的图片预览没有同样问题。现场附件 `144115250159782623-1793252969-1627947114.png` 为 945×2048；其中原图约位于 x=47…898、y=130…1973，整张屏幕截图被再次等比缩小，外层 13:49 状态栏与图内 13:47 状态栏同时出现。

## 黑边的直接调用链与差异

1. 此入口 `AIMobileScreenArchiveLargeImage.previewCanvas` 调用 `RemoteIMAsyncImage`。`ChatView.swift` 的 `RemoteIMAsyncImageState.load` → `RemoteIMImagePipeline.image` → 后台 `decode` → `maiImageDecodeFile`（`ChatView.swift:176`），再用返回的 RGBA 创建 `CGImage`、`UIImage`（同文件 204…215 行），最终由 SwiftUI `Image(uiImage:).scaledToFit()` 绘制。
2. `MaiChat/Media/MaiImageDecode.cpp:334` 是普通图片解码入口，使用 FFmpeg 的 libavformat/libavcodec/libswscale。428…434 行按原比例计算输出宽高，450…463 行只分配并写入该宽高的 RGBA，没有扩大画布补黑。它使用 FFmpeg 解码库，**没有启动 ffplay 播放器**。ffplay 的实际视频启动位于 `MaiFfplayVideoView.swift:390` 的 `startPlayback`，调用 `maiFfplayIosStartAtSpeed` / `maiFfplayIosStartVodAtSpeed`；本图片链路没有经过这些入口。
3. 已安装基线 `a07627edd` 的 `AIMobileScreenFramesView.swift:299` 用仍受 safe area 限制的 `GeometryReader`，只有 301 行黑色背景忽略安全区，306…312 行却按较矮的 `geometry.size` 缩放完整截图，且未隐藏外层状态栏。这会缩小整张屏幕图，产生现场四边留空和双重状态栏。
4. IM 的 `FullScreenImagePreviewView` 同样调用 `RemoteIMAsyncImage`，但 `ChatView.swift:4905` 对整个预览 `GeometryReader` 使用 `ignoresSafeArea()`；`RemoteIMImageHostingController` 在 2159 行隐藏状态栏，呈现方式为 overFullScreen。Session 的 `AIImagePreviewOverlay` 在 `AIAssistantView.swift:247` 对整个覆盖层忽略安全区、252 行隐藏状态栏。用户指出“只有这个新入口不同”有代码依据。
5. IM、Session 还复用 `ImagePreviewBackdrop`（`ChatView.swift:4773`），用同图柔化延展填充原图比例之外的余区；旧截图入口只有纯黑底。本轮直接复用这一组件，没有复制实现或更换解码器。

修正后，图片画布本身按完整 viewport 做等比适配、隐藏外层状态栏；关闭按钮仍在独立的安全区容器内。与屏幕同纵横比的原图可贴满四边；其他比例的图片保持完整内容，用共享柔化背景填充余区，不裁图或拉伸来撑满屏幕。

## 原图引用与清理生命周期

- 普通 C API 在既有档案核心上增加 `retain/release/assign_session/release_session`。每次保留取得独立 token，PNG 路径不变；Swift `retainSelected` 返回 `AIMobileScreenArchiveReference`，包含原 context、frame、referenceID 和原 fileURL。`AIImportedFile.archiveReference` 保存引用身份，relativePath 直接使用原 frame.path，不向 `screenshots` 再写重复图片。
- Agent 内部宿主已改为 `mobile_retain_screen_frame`，返回原 `file_path/path/reference_id/batch_id/frame_id`；旧 `mobile_export_screen_frame` 已删除。`mobile_screen_frames open` 与普通选图使用同一核心引用方案，不能绕过工作区/owner 边界；跨 owner 的宿主请求返回 `not_found`。
- 清理批次只删没有实际引用的原 PNG。仍被草稿、已发送会话或 Agent 历史引用的原图保留在档案位置，批次变为 retained 后从普通批次列表隐藏；后续释放最后一个 token 时才回收该原图及可回收目录。容量统计继续包含仍占用空间的 retained 图片，不因列表隐藏而漏算。
- UI 菜单改为“清理未选图片”。checkbox 尚未加入草稿时不等于持久引用：此时清理项禁用，并提示先把选图加入草稿，不自动导入，也不悄悄删除已勾选图片。关闭选择页仍保留本批。
- Model 导入后仍复核原账号、工作区和原附件 target；超限、取消或归属变化释放本次取得的 token。移除草稿附件释放该附件自己的 token，不影响同图的其他 token。发送前将引用绑定实际历史 Session，避免发送期间移除草稿导致正在发送的图片被删；已绑定历史的引用不因发送请求结果不确定而提前回收。清理历史或删除 Session 使用 `releaseSession` 释放该会话的持久引用。
- 本轮没有增加模型轮询。每 2 秒采集、停止保留、选图后用户主动发送的流程保持不变；跨账号不串图和单条消息最多 10 项附件的规则继续适用。

## 本地验证与亲看证据

- 核心 `mai_screen_frame_archive` **1/1 PASS，2.95 秒**，日志 `/private/tmp/maichat-screen-archive-references-test.log`，构建日志 `/private/tmp/maichat-screen-archive-references-build.log`。覆盖原图引用、独立 token、清理未引用文件、释放与会话回收等共享核心约束。
- iOS native Agent 契约编译和运行 PASS，日志 `/private/tmp/maichat-screen-frame-reference-agent-build.log`、`/private/tmp/maichat-screen-frame-reference-agent.log`。Agent open 取得原路径引用，清理批次不破坏已持有原图；旧复制宿主不再是正式路径。
- 模拟器 `ScreenFrameArchiveUITests/testRealSamplerSelectionUsesOriginalFramesAndCleanupKeepsReferences` **1/1 PASS，零跳过**，结果 `/private/tmp/maichat-screen-frame-reference-preview-20261029.xcresult`，日志同前缀 `.log`。设备为 iPhone 17 Pro / iOS 26.5；这不是 iOS 27 系统采集测试。素材仍走真实 sampler：前两张按设备 full viewport 像素尺寸生成，后两张为 320×180 横图。
- 同例断言纵图四边 frame 等于 App viewport、没有外层状态栏且关闭按钮可触达；横图保持 320:180 原比例。随后验证取消重开、未导入勾选时禁止清理、从 B 选择 A 的两张原图仍只加入 A 草稿、两个 UI token 与同图 Host token 各自独立、相对路径原样且 `screenshots` 没有新 PNG。清理后普通列表隐藏该批、未选原图删除、两张已引用原图仍有效；经 `Model.removeAttachment` 移除第一张后 Host 的同图仍可读，移除第二张后其原图回收，最后 `releaseSession("switch-A")` 后 Host 持有的最后原图也回收。异步回收采用最长 3 秒的条件等待。
- 已亲看本轮三份附件和录像 `/private/tmp/maichat-screen-frame-reference-preview-20261029.mp4`：纵图四边贴满，无额外黑框/外层时间栏；横图完整居中，上下是共享柔化背景；关闭按钮保持在右上安全位置。网格的纵横素材、两张选中状态、未导入时的禁用清理提示均清楚。最终录像显示“PASS: 原图无重复、独立引用安全、未选清理及会话回收通过”，此时 A=0、B=0 是测试已逐个释放附件后的预期结果。
- 导出目录 `/private/tmp/maichat-screen-frame-reference-preview-20261029-attachments/`：`D4C4942C-3F10-4FA4-A728-0F9BCF41DC1B.png`（纵图满 viewport）、`67FCB8F1-66C8-45B8-96DC-B443C6EEFB1F.png`（横图原比例与柔化背景）、`CABBB32A-7CE8-4C6E-93F9-D11A99C97239.png`（多选网格）。录像最终取帧 `/private/tmp/maichat-screen-frame-reference-final-result.png`。

## 132 真机采样已知事实与未验证范围

- 132 真机 QA 回执 `/private/tmp/maichat-screen-frame-physical-stopped.json` 报告 `ok=true`、`is_sharing=false`、批次 `stopped`、reason=`user_stop`、三帧 **1290×2796**，合计 **2,256,884 B**；current_app review 报告 `saved`。采样 elapsed_ms 为 4020、6357、8642，实际停止时间为 **2026-10-29 13:47:42.970 CST**。这是 132 的回执，不是 133 原图引用和预览修正的装机验收。
- Calculator launcher 文件的 mtime 为 **13:47:52**，晚于自动停止 **13:47:42.970**。因此不能把“发出了打开 Calculator 的操作”或该回执写成“已采集外部 App 内容”。目前只证明回执记录了三帧和停止/保存状态，尚未把这些原 PNG 回读解码、亲看并确认来源。
- 随后设备处于 unavailable，原 PNG 回读失败，错误码 4016；询问重连后设备恢复 connected。再次只读回取三张原 PNG 返回 7000（节点不存在），目前无法仅凭该错误确定清理原因，不声称原图仍可回读。
- 同批 current_app review PNG 回读成功，位置 `/private/tmp/maichat-screen-frame-physical-evidence/review.png`。执行者已亲看：原生“屏幕截图”选择页显示三张竖图缩略图，时间为 13:47:37、13:47:39、13:47:41，画面均为 MaiChat 会话。此证据证明停止后列表保留与显示；不能代替三张原 PNG 的逐张检查或外部 App 截图验收。
- 133 generic 构建退出 0，日志 `/private/tmp/maichat-screen-frame-ios133-build.log`；`codesign --verify --deep --strict` 通过。候选位于 `~/Library/Developer/Xcode/DerivedData/MaiChat-dpmznbhuepzeitfmwzvmmzmcdeql/Build/Products/Debug-iphoneos/MaiChat.app`，manifest `/private/tmp/maichat-screen-frame-ios133-candidate.json`。主可执行文件 91,856 B，SHA-256 `f13d1a62edca3f608e7fd26cae0acd0443c963ffac0dd40862b5686fb1b26bdf`；构建身份以 manifest 为准。
- 外部 App 的真实帧内容、133 正常手机选图原图引用与大图修正仍待最终合并版本集中验收；手机上仍是 132，不能把本地 133 构建成功写成已更新手机。

## 追加范围：云盘导航与全 App 栏位

- 用户明确要求移除 iOS 云盘重复的“返回上级”按钮，继续使用“我的云盘”和各级面包屑返回；没有要求删除目录浏览能力。删除的仅是重复按钮，根目录、祖先目录、子目录打开与空目录恢复路径继续保留。`CloudDriveFolderBrowseUITests/testFolderNavigationScopesMediaAndSupportsEmptyRefreshAndRemoteRemoval` 在本轮首轮模拟器回归中通过，包含长目录返回父级/根级、空文件夹、刷新及远端移除恢复。
- 半透明范围经用户明确为“整个 App 的上下栏”，本轮针对当前 iOS App：消息列表、单聊/群聊、创作中心与会话侧栏、云盘、通讯录、个人页、截图选图、视频圈及嵌入浏览器。正文需要真的滚到栏位下，不能仅把白色改成透明色而保持正文与栏位分区。保留键盘可读性、末项可达、草稿、引用、历史阅读位置；全屏媒体的独立预览不叠加普通 App 栏。
- 首轮 `build-for-testing` 成功，日志 `/private/tmp/maichat-ios-material-bars-build-20261029.log`。六个定向 UI 用例结果为三通过、三失败，零跳过，结果 `/private/tmp/maichat-ios-material-bars-20261029.xcresult`，录像 `/private/tmp/maichat-ios-material-bars-20261029.mp4`。这不是交付通过结果。
- 已通过的是 AI 长历史最新消息/键盘、AI 历史首尾滚动、云盘目录浏览。失败包括：正常 Root 的“创作中心”标签合成触摸未命中；IM 键盘打开后最新 2543 不在可见区域（截图停留 2537…2540）；截图选择测试关闭系统菜单时点击的说明文字已可滚动且不再可触达，尚未继续到引用清理收尾。截图大图在失败前已完成纵图满 viewport 和横图保留比例的断言，不能据此称整个选图用例已通过。
- 同时亲看发现 AI 材质栏后的文字透视仍需确认；修改安全区布局必须检查实际滚动 bounds、inset 与裁剪，不能只根据使用 `Material` 的源码下结论。首轮复现和后续修复、最终装机身份在本节继续补齐，不另立重复报告。

### 定点修复与验证证据

- 正常 Root 的失败活动日志记录“创作中心”合成触摸命中点为 `{-1,-1}`，AX 只报告 23×23 图标区域。材质背景改为 SwiftUI 原生 `background(Material, ignoresSafeAreaEdges:)`，主标签按钮补整格最少 44pt 的 `contentShape`；没有通过换测试点击坐标规避此问题。相同 `testAssistantMainTabsKeepDraftAcrossNavigation` 在 `/private/tmp/maichat-ios-material-bars-hit-20261029.xcresult` 红转绿，五页切换、键盘下主栏与草稿恢复均通过。
- AI 顶栏和输入栏中央的细竖线像素约为 221，对应背景 245 叠加 `barSeparator` 的 10% 黑色。原 `Divider` 没有限制尺寸，在外层横向布局中取了竖向；统一替换成 `RemoteIMBarSeparator` 的固定 0.5pt 横向矩形。新一轮截图 `...material-bars-follow...-attachments/75730357-820C-4707-95CD-6A0C605FFBF9.png` 已无该竖线。
- 滚动桥原先仅监听 contentOffset/contentSize/bounds，未监听 `adjustedContentInset`。新增该观察，并用 bounds 扣除四边 inset 得到有效可见高度来判断视口变化，定位 target 仍使用原 bounds 与 bottom inset，未重复扣除。另在桥接 UIView 加入进入 superview/window 后的挂载重试，拆卸清 hook，弱引用避免循环；不添加定时轮询或重建 ScrollView。
- 结构日志确认实际物理 frame：AI 滚动容器在窗口 y=0、height=874pt，adjusted top=116、bottom=101.3；IM 容器 y=62、height=778pt，adjusted top=52、bottom=67.3。二者 `clipsToBounds=true`。因此撤回仅凭内层 GeometryReader 的 656.7pt 高度推断“原生滚动视图整体被截短”的假设；安全区、可见区域和物理 bounds 不能混用。
- `/private/tmp/maichat-ios-material-bars-follow-20261029.xcresult` 的 AI 最新消息/键盘用例通过，IM 最新 2543 也已正确位于输入栏上方；同轮截图原图引用与选图清理完整用例通过，纵图满 viewport、横图原比例、勾选/导入/清理/引用释放全部跑到收尾。IM 用例仍因测试点击正文中央后等待收键盘失败，未将该轮整体标为通过。
- 收键盘手势会按设计避让 UITextView 和 UIControl，不能把中央坐标保证为“空白背景”。下一轮将背景点击定位至时间线右侧 padding，保留控件避让，并记录 touch 接受原因与 action 到达窗口；不为了断言而移除文字/日期/媒体交互保护。
- 视觉候选调整为标准 `.thin` / UIKit `.systemThinMaterial`，提高内容透视；消息页外画布承担安全区材质，内部白背景不再用 `ignoresSafeArea` 覆盖窗口边缘，导航 hosting/controller 原白底清空。没有改导航转场、消息与会话状态。最终视觉与运行结果待下节补齐。

### 最终模拟器结果（134）

- 最终栏位候选为 iOS **2.2.4 (134)**。`/private/tmp/maichat-ios-material-bars-final-20261029.xcresult` 中正常 Root 五页/键盘/草稿和 AI 长历史首尾滚动通过。已亲看正常 Root 九张截图及实际滚动录像取帧 `/private/tmp/maichat-ios-material-bars-final-contact.png`：标题、按钮、输入框清楚，滚动内容在薄材质后有宽柔影，中央竖线消失；正常消息页 status/home 原 255 纯白已变为约 240/242/244 和 239/244/245，与栏位连贯。Settings 只有两行且滚动前后截图相同，不能据此宣称验证了设置页长列表透视。
- 剩余 IM 失败最终定位为自动化选错滚动容器：活动日志 `/private/tmp/maichat-ios-im-final-activities.json` 明确 tap/swipe 目标为 `{x:0,y:538,width:402,height:45}` 的键盘候选栏，不是消息时间线，所以原生滚动桥没有收到该次触摸。这与产品手势拒绝文本控件是不同原因。保留原手势策略，给真实消息时间线增加 `im-message-timeline` 标识，让测试固定操作真实消息列表；临时 touch/action 诊断已经移除。
- 修正选择器后，相同 IM 用例在 `/private/tmp/maichat-ios-material-im-timeline-20261029.xcresult` **1/1 PASS，零跳过**。完整覆盖最新 2543 在键盘/输入栏上方、点击背景收键盘、上滑打开更早 2504、返回会话列表且可交互。执行者亲看两张附件：`BF950313-0359-48DE-BEBC-55680AC453B5.png` 中 2543 最后一行完整显示；`81C27102-EA3A-4AB7-A68F-D350221BD25D.png` 已显示更早 2525…2531，键盘关闭，未出现空白视口。不能把此前两张相同的失败截图算作分页证据。
- 前述档案核心、Agent 原图引用契约和选图完整 UI 用例仍为相应源码已通过的独立证据，不把这些不同构建合成一次“所有测试全绿”的虚构运行。后续 generic 真机构建、安装与设备回读在同一记录继续追加；本节只证明模拟器实际运行范围，不代替 iOS 27 整机共享、外部 App 帧内容和全套真机手势验收。

### 134 真机构建与安装回执

- 产品提交 `0ea4cca4888956f8f634bec19f9d45c3cabb781c`，generic iOS Debug 构建退出 0，日志 `/private/tmp/maichat-ios-material-bars-device134-build.log`；`codesign --verify --deep --strict` 通过。清单 `/private/tmp/maichat-ios-material-bars-134-candidate.json`。主文件 91,856 B、SHA-256 `00adeacb3262a96c051b55a96a7b5de9d630ec6844dda8c97c6fdb5a6fd02295`；`MaiChat.debug.dylib` 86,513,728 B、SHA-256 `89c7d169f649087e41491097275fdeb6b2a7d897c408c5592dca72c79789e859`。
- 同一模拟器构建额外运行既有 `--ai-browser-ui-test` 普通 API 验收，结果 `/private/tmp/maichat-ios-material-browser-134-result.txt` 为 `PASS: observe, type, click, verify, screenshot`，当次新写入结果与启动时间匹配。执行者亲看 `/private/tmp/maichat-ios-material-browser-134.png`：嵌入网页标题、输入的 `iOS WebKit OK`、确认按钮与“操作已完成”均显示，网页首行没有被标题盖住。这里只证明该短测试页与既有工具契约，没有声称已验长网页末项。
- 2026-10-29 15:46 CST 左右，iPhone 14 Pro Max / iOS 27.2（UDID `00008120-001E0DEC0263C01E`）覆盖安装成功，安装回执 `/private/tmp/maichat-ios-material-bars-134-install.json`，安装路径 `/private/var/containers/Bundle/Application/D40C7CC8-2244-4FAE-AA91-EC473BAD3BAA/MaiChat.app/`。独立 `device info apps` 回读 `com.kongshang.maichat` = **2.2.4 (134)**，回执 `/private/tmp/maichat-ios-material-bars-134-installed-app.json`；没有卸载 App 或清理用户数据。
- 随后启动被系统拒绝，CoreDevice 10002 → FBSOpenApplicationErrorDomain 7、`Locked`，明确为手机锁屏而非启动崩溃。启动回执 `/private/tmp/maichat-ios-material-bars-134-launch.json`。已请求用户解锁后继续，当前不能把“安装成功”写成“真机已启动并完成 UI 验收”。独立 `idevicescreenshot` 也因 screenshotr service 不可用失败，未获得真机截图，不把模拟器截图当真机画面。

## 134 回归：聊天滚动闪动、顶部灰白分层，135 收敛修正

- 用户于 2026-10-29 15:57 左右反馈聊天上下滚动“一闪一闪”，且状态栏和标题栏出现灰白两色；同批要求删除云盘“我的文件”文字。现场图 `144115250159782623-1793260624-4129811858.png` 的状态区至标题栏分界在原图 y=177（59pt×3），相邻行 median 从约 `[241,242,243]` 变为 `[235,235,236]`；这是明确的颜色硬切，不是仅凭文字猜测。
- 真机只读日志已取回 `/private/tmp/maichat-ios134-physical-flicker.log`（约626KB）。实际 134 在 15:55:16 CST 已启动，launch=`049e4390`、iOS27.2，更新了此前“仍待解锁启动”的历史状态。15:48–15:59 窗口内 position/dismiss 桥各安装一次，无反复拆装；四次 offset clamp 中三次紧随键盘开关，没有普通滚动期间持续改 offset 的证据，不将滚动读取器认定为本次闪动根因。
- 最近产品提交 `0ea4cca48` 为 ChatView 整页添加 `.thin Material`，标题栏和输入栏又各有一层 Material。IM UIKit host 以系统安全区起点布局，各层采样不在同一画布中；该源码叠加与现场 y=177 的灰白分界吻合。另 SwiftUI ShapeStyle 的 `.background` 默认会忽略安全区，省略参数不能保证白色内容背景限于中间区域。
- 修正收敛为背景样式：共享栏底改为动态 `systemBackground`、固定0.9 alpha，原生导航/标签栏 `backgroundEffect=nil`，不再动态模糊采样；IM 整页画布与 hosting 背景恢复稳定实色，内部白色/page 背景显式 `ignoresSafeAreaEdges: []`。本轮没有再改布局、导航转场、滚动意图或校正策略。云盘只删除 `sectionTitle("我的文件")` 一行，保留容量、面包屑、文件夹条目及上传区域。
- 基线134按同一长 IM 手势录屏 `/private/tmp/maichat-ios134-im-flicker-baseline.mp4`，用例通过只能证明动作完成；静态灰白带可见，但未在这段模拟器录屏重现用户描述的整页闪白，不声称已用模拟器完整复现所有真机闪动。
- 135 模拟器定向回归 `/private/tmp/maichat-ios135-im-background-fix.xcresult` **3/3 PASS，零跳过**：正常 Root 五页/草稿/键盘且“我的文件”不存在；展开长历史快速滑动后慢速上、下滑；最新消息/键盘/背景收键盘/历史分页/返回列表。录像 `/private/tmp/maichat-ios135-im-background-fix.mp4`，快滑约59.277秒、慢上滑67.232秒、慢下滑70.274秒。执行者亲看两段8fps密集取帧 `/private/tmp/maichat-ios135-flicker-fast-contact.png`、`...slow-contact.png` 及大图：取帧内内容连续，上下栏无整条灰白变换，状态/标题底色连贯，标题和按钮仍清楚，正文从半透明底色后透出。采样取帧不等于对全部原始帧的穷尽检查。
- 135 的 `7DCA0FE8-07BB-4FFA-95C4-2CE7748ADB5E.png` 显示键盘打开时最新2543完整在输入栏上方；云盘附件 `98B589C4-AB43-4383-B9F4-7922DFA0686B.png` 已无“我的文件”。135 真机构建、安装与运行回执后续追加；目前尚未把新版本真机滚动视觉标为已验收。
- 135 静态复核补充：同系统安全区边界，正常消息页分界两侧均为 `[255,255,255]`，IM 详情最大约0–1 RGB差，旧整条灰带消失。顶部/输入栏下淡化正文约 `[231,232,233]`，与深色正文叠90%白的单层 alpha 相符，证明未以不透明白色盖住正文来隐藏问题；标题和按钮仍清楚。
- 产品提交 `61d2429783dfd0bebf8139e85b3cdec525185bab`，generic iOS Debug 构建退出0、深度签名核验通过，日志 `/private/tmp/maichat-ios135-background-device-build.log`，清单 `/private/tmp/maichat-ios135-background-candidate.json`。主文件91,856B、SHA-256 `222defeb92e0b933fe511fbffddcbf23ba87d1e5b52f3852b21fafd8dda546b2`；调试 dylib86,509,600B、SHA-256 `f0b08e98db58baae8e78f7a2ccd79db573713a6381aa142a56101a72f5e7af9f`。
- 已覆盖安装到同一 iPhone14ProMax/iOS27.2，独立 apps 回读2.2.4(135)。安装路径 `/private/var/containers/Bundle/Application/878CBED9-82B5-404A-AD6E-523547BDC9CA/MaiChat.app/`，回执 `/private/tmp/maichat-ios135-background-install.json`、`...installed-app.json`；随后普通无参数启动成功，实际 executable 与安装路径一致，PID5932，回执 `...launch.json`。App自身日志 `/private/tmp/maichat-ios135-background-runtime.log` 确认 `2026-10-29T08:20:12.203Z app_version=2.2.4 build=135 ios=27.2 pid=5932`。本次未卸载或清理用户数据，未触发模型请求或系统共享面板。
- 验证边界：135模拟器同操作和密集取帧通过、真机安装与正常启动已核对；135真机连续滚动的视觉尚未取得录像复核。此前真机 UI Automation 已明确在初始化阶段被权限阻止，本轮未重复同一失败初始化；也没有将模拟器画面称为真机验收。灰白分层的原因已证实，不能凭这些证据宣称穷尽了用户所有真机闪动情形。

## 136：三个会话页面的底部安全区分层与云盘入口精简

- 用户明确消息列表、好友聊天详情、创作中心存在透明/不透明分层，并要求减弱透出；云盘、好友列表效果满意，不能继续全局改色。现场消息列表图 `144115250159782623-1793262831-883741438.png` 的后方头像/文字在底栏可见，至约 y2690 后变成一段纯白，接近该机 home 安全区起点；只提高 opacity 不能修掉承载范围缺失。
- 消息页同一个主底栏经 `AnyView` 放入 ChatNavigationHost 内部的 UIHostingController，135 实际原生滚动容器 window y62、height778、maxY840，距模拟器窗口874少34pt；云盘/好友列表的底栏直接由 Root inset 持有，因此表现不同。136仅将 portrait 的 ChatNavigationHost 延伸进 `.container` 的底部安全区，不改导航方法/转场/缓存；新日志 y62、height812、maxY874、bottom inset101.3，证实补到了窗口底部且保留了控件安全距离。
- 创作中心内部 composer 的背景此前同样忽略 `.bottom`，越过自己的区域继续画到外层 RootTab/home，可能与 RootTab 的0.9背景叠至0.99；136取消该内层背景的底部延伸，页面/抽屉底色明确限于自身区域，仅RootTab负责窗口底部背景。
- 透明度仅局部调整：消息列表/详情、创作中心的 header/composer/tab 改为0.97，内容透出从10%降至3%；云盘、好友列表、原生导航外观以及其他helper调用保持0.9。没有引入模糊或再修改滚动策略。
- 新增模拟器隔离数据入口复用实际 RootView、ChatView、AIAssistantView 与真实 RootTabBar，装载35个联系人、80条聊天及80条AI记录；用于覆盖有内容滚到home后方的场景，补足此前短列表/占位底栏验证范围。只在Simulator显式测试参数下可达，不进入手机普通启动路径。`/private/tmp/maichat-ios136-safe-area.xcresult` **3/3 PASS，零跳过**：三页容器 maxY 与窗口底部一致、主Tab控件仍在home上方；IM键盘/最新/分页/返回通过；云盘长目录、根/父级返回及空目录恢复通过。
- 已亲看 `/private/tmp/maichat-ios136-safe-area-attachments/` 的 `4F596CFD-1DD2-4C5E-AD83-9D5C34F45D4A.png`、`7937E796-5791-4726-9D50-5229F0FCBD83.png`、`3BA6C0A5-9F35-4047-916D-B77D7F2FF324.png`：三页长内容能在同一较淡底色后延续至屏幕底边，输入框/Tab区域无此前明显透度断带，按钮位置和文字清晰。录像 `/private/tmp/maichat-ios136-safe-area.mp4`；这些是模拟器证据。
- 用户追加的云盘小改：根目录隐藏重复蓝色“我的云盘”路径整行，保留顶部标题；子目录继续保留根/祖先导航。将外层栈作为根滚顶锚点，避免隐藏路径后失去返回根目录的滚顶目标。视频行单项“保存到相册”菜单改为直接下载图标按钮，复用同一saveVideo、进度、取消/错误与44pt命中区；保留无障碍标签，屏幕不再弹一项大文字菜单。子目录截图 `DA7C4043-62CA-4399-B1C8-FD15C8D7DA77.png` 可见下载图标；没有新增移动端文件夹管理控件。
- 真机版本/安装/启动和 DEBUG导航容器几何回执在构建后继续补齐；未把本轮模拟器长内容通过写为真机连续手势视觉已验收。
