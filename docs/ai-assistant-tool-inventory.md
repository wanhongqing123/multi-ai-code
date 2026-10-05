# AI 助手工具清单

本清单按代码中注册给 MaiAgent 主模型的**工具名**统计，核对位置为 2026-10-05 的当前工作区。相同工具在 iOS、Android、桌面重复注册只算一个；工具的参数、模式和子命令不另计。它统计的是源码可注册的上限，**不等于每台设备都能成功调用**：编译开关、模型文件、系统权限、API Key 和供应商授权仍会影响实际可用性。

| 口径 | 工具名数量 | 说明 |
|---|---:|---|
| iOS AI 助手 | 70 | 含 APP 存储管理及 iOS 专属相册工具；视频抠像需模型资源 |
| Android AI 助手 | 69 | 暂不注册 APP 存储清理；包含 Android 专属相册工具，尚未做本轮构建验证 |
| Qt 桌面 AI 助手 | macOS 最多 64；Windows 最多 65 | 含 APP 存储管理与 GPU 信息；Windows 暂保留两个类型化 OpenSSL 工具，桌面视觉等受编译条件限制 |
| 各平台合并去重 | **83** | Windows 旧 OpenSSL 名称与 Mac/移动端新 `openssl` 名称分别计入 |

本轮将四个 `curl_*` 模型工具合并为复用上游命令入口的 `curl`；Mac/iOS/Android 将两个类型化 OpenSSL 模型工具合并为复用上游只读子命令的 `openssl`。类型化 EVP/X509 函数仍可供代码直接调用；Windows 仍注册原有两项，等待 CLI 嵌入端口。新增 `system_resources`、`network_ip`，并在 iOS 增加 `app_storage`。原有的 ffmpeg、ffprobe 扩展了能力；已删除的 mobile_beautify_face、尚在调研的视频超分不计入。

## 各端主要共享工具

### 文件和代码：9 个

注册入口：移动端 [MaiMobileAgent.cpp](../MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp)；桌面端 [MaiTool.cpp](../MaiAgent/AgentTools/MaiTool.cpp) 与 [AgentController.cpp](../MaiChat/desktop/src/agent/AgentController.cpp)。实现位于 MaiAgent/AgentTools/。

| 工具 | 用途 | 实现文件 |
|---|---|---|
| file_read | 按行读取文本文件，处理 UTF-8、带 BOM 的 UTF-16 和指定的旧编码。 | MaiFileTools.cpp |
| file_create | 创建空文件及缺失的父目录，不覆盖已有文件。 | MaiFileTools.cpp |
| file_create_directory | 递归创建目录。 | MaiFileTools.cpp |
| file_delete | 删除一个已有文件，不递归删除目录。 | MaiFileTools.cpp |
| file_write | 将文本写入文件，可替换已有内容。 | MaiFileTools.cpp |
| file_edit | 精确匹配并替换文件中的一段文本。 | MaiEditTool.cpp |
| file_patch | 按补丁一次修改多个位置或文件。 | MaiApplyPatchTool.cpp |
| file_glob | 按文件名模式查找文件。 | MaiFileTools.cpp |
| file_grep | 在文本文件中用正则表达式搜索内容。 | MaiFileTools.cpp |

### 网络、压缩与证书

`curl` 由 [MaiCurlTools.cpp](../MaiAgent/AgentTools/MaiCurlTools.cpp) 注册，[MaiCurlCliTool.cpp](../MaiAgent/AgentTools/MaiCurlCliTool.cpp) 适配上游命令入口；zlib、OpenSSL 源码分别在 [third_party/zlib](../MaiAgent/third_party/zlib/) 与 [third_party/openssl](../MaiAgent/third_party/openssl/)。

| 工具 | 用途 | 实现文件 |
|---|---|---|
| curl | 复用上游 curl 命令入口，支持有界 HTTP(S) 请求、网页提取、原始文件上传和新文件下载；参数和路径受限，逐次确认。 | MaiCurlCliTool.cpp |
| zlib_compress | 把工作区文件压成 gzip、zlib 或原始 deflate 新文件。 | MaiZlibTool.cpp |
| zlib_decompress | 解压 gzip、zlib 或原始 deflate；源文件不变。 | MaiZlibTool.cpp |
| openssl | Mac/iOS/Android：复用上游命令入口，只开放 version、受限 dgst、受限 x509 只读命令。 | MaiOpenSslCliTool.cpp |
| crypto_digest | Windows 暂保留：用 EVP 计算文件或短文本摘要。 | MaiOpenSslTool.cpp |
| crypto_certificate_info | Windows 暂保留：解析 PEM/DER X.509 证书。 | MaiOpenSslTool.cpp |
| network_ip | 查询设备活动网卡的局域网 IPv4/IPv6；可选经 ipify HTTPS 查询公网出口 IP。 | MaiNetworkIpTool.cpp |
| system_resources | 读取 CPU 时间、CPU 核数、物理内存、进程驻留内存；iOS/macOS 读取 Metal GPU 分配量及建议预算，Windows 读取 DXGI 显存预算与进程占用。 | MaiSystemResourcesTool.cpp；桌面 GPU 实现位于 MaiChat/desktop/src/agent/MaiDesktopGpuMac.mm、MaiDesktopGpuWindows.cpp |
| app_storage | iOS 与桌面：扫描 APP 临时/缓存/工作区占用；经预览和逐次确认后只清理至少 24 小时未修改的临时及缓存文件。 | MaiAppStorageTool.cpp |

### 主模型交互与图片查看：4 个

| 工具 | 用途 | 实现文件 |
|---|---|---|
| todowrite | 记录并更新当前任务列表。 | MaiAgent/AgentTools/MaiTodoWriteTool.cpp |
| question | 在一轮执行中向用户提问并等待答案。 | MaiAgent/AgentTools/MaiQuestionTool.cpp |
| current_time | 读取当前时间。 | MaiAgent/AgentTools/MaiTimeTool.cpp |
| view_image | 查看可访问的图片；移动端和桌面端会注入各自的预览后端。 | MaiAgent/AgentTools/MaiViewImageTool.cpp |

### 媒体处理与交付：6 个

| 工具 | 用途 | 实现文件 |
|---|---|---|
| agent_send_media | 把工作区图片、视频或音频作为卡片投递到**当前 AI 助手会话**；不发给 IM 联系人。 | MaiAgent/AgentTools/MaiAgentSendMediaTool.cpp |
| ffmpeg | 在进程内转换本地音视频和图片，支持滤镜等 FFmpeg 参数；本次增加诊断日志捕获。 | MaiAgent/AgentTools/MaiFfmpegTools.cpp |
| ffprobe | 读取容器、音视频流、章节、帧、关键帧和包信息，含颜色/HDR 等元数据。 | MaiAgent/AgentTools/MaiFfmpegTools.cpp |
| video_stretch_lower | 保留画面上部，对视频下部定向拉伸并拼接，适合局部比例调整；限定支持的 SDR 输入。 | MaiAgent/AgentTools/MaiVideoGeometryTool.cpp |
| cv_video_matting | 用 RVM ONNX 对视频人物抠像并更换、模糊背景，生成新视频；仅在运行时资源可用时注册。 | MaiAgent/AgentTools/MaiVideoMattingTool.cpp |
| generate_pdf | 将 Markdown 内容生成 A4 PDF；渲染由各端宿主提供。 | MaiAgent/AgentTools/MaiPdfTool.cpp |

### OpenCV 分析：11 个

视频工具实现：[MaiCvVideoTools.cpp](../MaiAgent/AgentTools/MaiCvVideoTools.cpp)，计算核心为 [MaiCvVideoAnalysis.cpp](../MaiAgent/AgentTools/MaiCvVideoAnalysis.cpp)；图像工具实现：[MaiCvImageTools.cpp](../MaiAgent/AgentTools/MaiCvImageTools.cpp)，计算核心为 [MaiCvImageAnalysis.cpp](../MaiAgent/AgentTools/MaiCvImageAnalysis.cpp)。

| 工具 | 用途 |
|---|---|
| cv_scene_detect | 检测视频镜头切换，返回时间段。 |
| cv_motion_detect | 检测视频中有明显运动的时间段；不识别人。 |
| cv_image_quality | 量化图像锐度、亮度、对比度与高光/阴影裁切。 |
| cv_image_compare | 比较两张同尺寸图的像素差异、变化比例、PSNR 和 dHash 距离。 |
| cv_find_contours | 找外部轮廓及其矩形边界。 |
| cv_detect_edges | 生成 Canny 边缘图 PNG。 |
| cv_match_template | 在大图中查找同尺度的小模板图。 |
| cv_register_translation | 估算两张图之间的水平、垂直平移量。 |
| cv_detect_lines | 检测直线段并估算近水平线的倾斜角。 |
| cv_document_corners | 定位清晰文档四边形的四个角。 |
| cv_threshold_mask | 生成固定、自适应或 Otsu 阈值的二值掩码 PNG。 |

### 专业模型：5 个

火山方舟实现：[MaiArkMediaTools.cpp](../MaiAgent/AgentTools/MaiArkMediaTools.cpp)；阿里百炼实现：[MaiModelStudioTools.cpp](../MaiAgent/AgentTools/MaiModelStudioTools.cpp)。这些工具注册后仍可能因密钥、Workspace、额度、模型授权或内容审核而不可用。付费的 delegate/revise 每次需要确认；后台完成后会把最终回复交给主模型，continue 仅用于手动排障。

| 工具 | 绑定模型与用途 |
|---|---|
| seedance_video | Seedance 2.0：文生视频、首尾帧、已完成任务或 HTTPS 视频引用的编辑/延长；**本地视频直接上传未接通**。 |
| seedream_image | Seedream 5.0 Flash：文生图、单图编辑、多图合成一张 PNG、按旧任务修订。 |
| wan_video_edit | 万相 2.7 视频编辑：上传本地视频，按文字指令编辑，可带参考图片；当前临时上传方式用于开发测试。 |
| wan_video | 万相 3.0 视频生成：文生视频、首尾帧、单个视频和参考图驱动的生成/编辑。 |
| qwen_image | Qwen-Image-3.0-Pro：文生图、最多三张本地图编辑/合成、基于上一版修订。 |

### SSH：1 个

| 工具 | 用途 | 实现 |
|---|---|---|
| ssh_exec | 以账号密码连接云主机并执行**单条命令**；密码由宿主原生对话框输入，校验主机指纹，不写入模型参数。 | [MaiSshTool.cpp](../MaiAgent/AgentTools/MaiSshTool.cpp)；iOS [AIMobileSshTools.swift](../MaiChat/ios/MaiChat/MaiChat/MaiChatTools/AIMobileSshTools.swift)、Android [MobileSshTools.java](../MaiChat/android/MaiChat/app/src/main/java/MaiChatTools/com/kongshang/maichat/tools/MobileSshTools.java)、桌面 [MainWindow.cpp](../MaiChat/desktop/src/ui/MainWindow.cpp) 提供交互 |

### IM 会话：9 个

移动端注册：[MaiMobileHostTools.cpp](../MaiChat/MaiChatTools/MaiMobileHostTools.cpp)；桌面端注册及实现：[MaiChatHostTools.cpp](../MaiChat/desktop/src/MaiChatTools/MaiChatHostTools.cpp)。移动端具体调用转接到 iOS/Android 的 MaiChatTools 实现。

| 工具 | 用途 |
|---|---|
| maichat_list_contacts | 按 ID 或名称列出联系人。 |
| maichat_list_conversations | 按最新消息列出会话及未读数量。 |
| maichat_get_messages | 读取指定联系人的近期消息；图片消息带确定的工作区路径。 |
| maichat_search_messages | 在一个联系人或所有联系人消息中搜索文字。 |
| maichat_get_unread_summary | 按联系人汇总未读消息。 |
| maichat_send_text | 给联系人发送文字，需用户确认。 |
| maichat_send_media | 给联系人发送工作区图片、视频或音频，需用户确认；与 agent_send_media 的收件人不同。 |
| maichat_reply_message | 引用某条 IM 消息回复文字，需用户确认。 |
| maichat_broadcast_text | 向多个联系人发相同文字，每次需用户确认完整收件人列表。 |

### 视频播放：2 个

移动端注册于 [MaiMobileHostTools.cpp](../MaiChat/MaiChatTools/MaiMobileHostTools.cpp)，桌面端实现于 [DesktopMediaTools.cpp](../MaiChat/desktop/src/MaiChatTools/DesktopMediaTools.cpp)；桌面端需 FFplay 编译开关。

| 工具 | 用途 |
|---|---|
| maichat_play_video | 打开本地视频播放浮窗。 |
| maichat_video_command | 控制当前播放器的播放、暂停、逐帧、跳转、音量、全屏等。 |

## 桌面端／CLI 额外工具：10 个

其中子 Agent 和 shell/截图工具来自 [MaiAgent/AgentTools](../MaiAgent/AgentTools/)；桌面视觉工具位于 [DesktopVisionTools.cpp](../MaiChat/desktop/src/MaiChatTools/DesktopVisionTools.cpp)。这些不在移动端 AI 助手清单中。

| 工具 | 用途 | 实现文件 | 平台 |
|---|---|---|---|
| spawn_agent | 启动一个独立子 Agent 会话处理子任务。 | MaiSubAgentTools.cpp | 桌面、CLI |
| wait_agent | 等待子 Agent 当前工作结束并取得回复。 | MaiSubAgentTools.cpp | 桌面、CLI |
| send_input | 给已有子 Agent 发送后续指令。 | MaiSubAgentTools.cpp | 桌面、CLI |
| list_agents | 查看子 Agent 的运行、空闲和关闭状态。 | MaiSubAgentTools.cpp | 桌面、CLI |
| close_agent | 关闭不再需要的子 Agent。 | MaiSubAgentTools.cpp | 桌面、CLI |
| screenshot | 截屏或截窗口供模型查看；只在平台截图能力可用时注册。 | MaiScreenshotTool.cpp | 桌面、CLI（条件） |
| list_windows | 列出可供窗口截图的顶层窗口；条件同上。 | MaiWindowListTool.cpp | 桌面、CLI（条件） |
| shell | 在工作目录运行 shell 命令；移动端沙箱不注册。 | MaiShellTool.cpp | 桌面、CLI（条件） |
| detect_faces | 桌面端图像人脸框与关键点检测，不识别身份。 | DesktopVisionTools.cpp | 桌面（条件） |
| segment_person | 桌面端人物分割并输出灰度掩码 PNG。 | DesktopVisionTools.cpp | 桌面（条件） |

## 移动端额外工具：17 个不同名称

iOS、Android **各注册 16 个**：下面共同的 15 个，再加各自的一种相册操作。移动工具 C++ 壳注册在 [MaiMobileHostTools.cpp](../MaiChat/MaiChatTools/MaiMobileHostTools.cpp)；实际系统权限、相册、图片处理、预览在 [iOS MaiChatTools](../MaiChat/ios/MaiChat/MaiChat/MaiChatTools/) 与 [Android MaiChatTools](../MaiChat/android/MaiChat/app/src/main/java/MaiChatTools/)。

| 工具 | 用途 | 平台 |
|---|---|---|
| mobile_request_permission | 请求当前任务所需的相册、相机、麦克风、定位、联系人、日历或通知权限。 | iOS、Android |
| mobile_get_location | 获得一次前台当前位置与精度信息。 | iOS、Android |
| mobile_list_photos | 分页列出系统相册可访问的图片、实况照片和视频。 | iOS、Android |
| mobile_list_albums | 列出系统相册。 | iOS、Android |
| mobile_read_photo | 复制不超过 2048px 的 JPEG 预览图到工作区；不适合作为保真编辑源。 | iOS、Android |
| mobile_export_photo_original | 按相册 ID 导出原始照片字节，实况照片可选配对视频。 | iOS、Android |
| mobile_export_media_original | 通用原始照片/视频导出，保留格式和可用元数据。 | iOS、Android |
| mobile_save_image | 将工作区图片作为新照片存入系统相册。 | iOS、Android |
| mobile_save_video | 将工作区 MP4/MOV/M4V 视频作为新视频存入相册，源文件不变。 | iOS、Android |
| mobile_transform_image | 本机裁切、旋转、缩放、翻转、灰度、锐化、亮度/对比度调整或轻度整图美化。 | iOS、Android |
| mobile_beautify_image | 轻度整图平滑、提亮与色彩调整；**不是人脸关键点级美颜**。 | iOS、Android |
| mobile_image_info | 查询图像格式、尺寸和文件字节数。 | iOS、Android |
| mobile_detect_faces | 本机检测人脸框与关键点，不识别身份。 | iOS、Android |
| mobile_segment_person | 本机人物/背景分割，输出灰度掩码图。 | iOS、Android |
| mobile_preview_image | 用手机原生全屏界面预览工作区图片。 | iOS、Android |
| mobile_photos_add_to_album | 将已有照片 ID 加入或新建 iOS 相册，不复制原图。 | 仅 iOS |
| mobile_photos_copy_to_album | 将已有照片复制到 Android 图片文件夹形成相册，原图保留。 | 仅 Android |

平台专属相册注册代码：[iOS](../MaiChat/ios/MaiChat/MaiChat/MaiChatTools/MaiMobilePhotoAlbumTool.cpp)、[Android](../MaiChat/android/MaiChat/app/src/main/cpp/MaiChatTools/MaiMobilePhotoAlbumTool.cpp)。

## 目录与口径说明

- 共享工具的公开声明放在 MaiAgent/include/，实现放在 MaiAgent/AgentTools/；总注册入口在 MaiAgent/AgentTools/MaiTool.cpp，文件和 curl 工具组分别由 MaiFileTools.cpp、MaiCurlTools.cpp 统一注册。模型会话、自动交接与持久化逻辑在 MaiAgent/Agent/，**不算工具**。
- 移动端注册入口为 MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp，平台宿主工具壳为 MaiChat/MaiChatTools/；iOS、Android 的原生实现位于各自 MaiChatTools 目录。
- 桌面端注册入口为 MaiChat/desktop/src/agent/AgentController.cpp，IM、播放和视觉宿主实现位于 MaiChat/desktop/src/MaiChatTools/；MainWindow.cpp 注入 SSH 原生密码和指纹确认。
- CLI 注册 MaiAgent 的内置工具；截图、窗口列表及 shell 还需运行平台支持。第三方库源码本身不是额外的模型工具。
