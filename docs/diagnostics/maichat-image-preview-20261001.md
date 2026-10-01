# MaiChat 消息图片小图显示失败（2026-10-01）

## 现场与复现

- 用户在 macOS 的 IM 会话中看到消息气泡显示“图片暂不可预览”，点击该气泡进入放大窗口后，同一张图片能够显示。
- 现场文件是有效的 3024×1898 PNG，位于系统临时目录的 `maichat-paste` 下。放大窗口能显示，说明文件并非缺失。
- 故障版本的小图经 Graphics 路径解码；放大窗口经 Qt 图片路径解码。不能把两个路径的结果当成同一条解码链。

## 定位证据

- 最近的 `2d48e1b6` 删除了 Mac 消息小图原来的 Qt 显示路径，改成 Graphics 覆盖层。
- 当时 FFmpeg 构建使用 `--disable-autodetect` 且未启用 zlib，配置里的 `CONFIG_PNG_DECODER` 为 0。用现场 PNG 运行 Graphics 测试得到“no decoder”；启用 zlib 后为 1，同一文件通过测试。
- 用户明确要求：普通消息和 AI 助手的小图由 FFmpeg 解码成像素，交给 Qt、UIImage 或 Android Bitmap 显示；Graphics 用于放大的渲染视图。因此只补 PNG 解码器还不足以修复错误的显示路径。

## 修复与验证

- `cb4c3412` 为 Mac、iOS、Android FFmpeg 启用 PNG 解码，增加共用的首帧 RGBA 解码接口，并将三端小图接回各端原生图片控件。Mac 与 Android 的放大视图仍可走 Graphics。
- Mac：共用 FFmpeg 解码测试和真实消息气泡的 QLabel 显示用例通过；现场 PNG 的 Graphics 测试通过。iOS：Release 模拟器构建通过。Android：Debug App 构建通过；Pixel 9 模拟器的 PNG 小图像素检查和 Graphics 呈现测试通过。
- 免安装 Mac App 从该提交构建并签名，路径为 `MaiChat/desktop/dist/local-preview-cb4c3412/MaiChat-macos-arm64/MaiChat.app`。直接启动后核对进程的可执行文件位于此路径，启动日志显示版本 0.1.96。
- 失败日志记录路径或稳定标签、目标尺寸和 FFmpeg 错误码，以便以后定点排查。

## 未验证边界

- iOS 只有模拟器构建，尚无 iPhone 真机图片回归结果。
- Windows 保留原 Qt 小图路径；该提交未为 Windows 的 FFmpeg 构建启用 PNG 解码。
- 这个提交不包含 ffplay 全量媒体播放工具。
