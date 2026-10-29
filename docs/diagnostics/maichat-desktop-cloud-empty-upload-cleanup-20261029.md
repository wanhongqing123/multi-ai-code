# Desktop 云盘冗余标题与空续传区（2026-10-29）

## 复现与修改

- 用户截图 `1793276011-2149791887.png`：容量卡下方的“待续传”“没有待续传的文件”“我的图片和视频”及空白区域无须常驻。当前源码的第三个标题已变为“文件夹与文件”，同样移除。
- `CloudDriveDialog` 构造时隐藏续传滚动区；刷新为空时保持隐藏，有真实检查点时才显示原任务行、继续/放弃及高度。账号切换也通过同一空渲染路径收起旧区域，不删除持久检查点。
- 此改动仅收敛 Desktop 展示；原有文件夹导航、媒体、上传及恢复实现保持共用，不新增移动端管理按钮。

## 验证

- macOS 27.2 / Qt 5.15.18 / Cocoa，源码基点 `3d503e071` 加本轮修复。`cloud_drive_dialog_local_tls` **1/1 PASS，7.99 秒**。隔离本地 TLS 测试账号，无生产账号或真实云盘数据变更。
- 实际继续上传通过，包括重新定位原文件、账号切换取消旧 worker、文件夹和媒体操作。放弃按钮仅核对展示，没有点击。
- 已亲看 `/tmp/maichat-cloud-review.UdoiXU/cloud-empty.png`、`cloud-resume.png`、`cloud-folders-with-file.png`：空区收起；非空任务的文件名、继续与放弃保留；目录和媒体可见。日志同目录 `ctest.log`。
- 执行该次测试的源文件 SHA256：`824dc8dd6fbae2a6eba3ad77f093b491b3e5535eac19cb38506c11335c7bd271`；测试程序 SHA256：`245ae882ebfb4b8e903267c111061407df4d2b8ee5d8ad7f6365a927ab147cb6`。
- 测试不是最终统一安装包，未替换现装 App；最终版本与产物回读另行追加。Windows 构建未验证。

## 统一构建记录

修复提交 `6e97705d4d2bafd203a523dae3e8d4ab1a1a6388`，已推送 main。统一 Desktop **2.2.32** staging 完成签名和裸 PATH `--smoke`（exit 0）；主程序 SHA256 `fcda1b0186456ab12c6703877cc95e045df815559945d56d816c265eb7e2644d`。主执行者已查看上述修后截图。现装 2.2.31 尚未替换，具体产物与用户确认状态见 `maichat-model-stream-protocol-liveness-20261029.md` 的完成记录。
