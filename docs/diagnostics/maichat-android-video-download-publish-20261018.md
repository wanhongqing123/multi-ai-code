# Android 视频下载在发布文件时失败（2026-10-18）

## 现场与复现

- 平台：OPPO PKD130，Android 15，MaiChat 已发布签名包 2.2.1 (221)。
- 用户在创作中心查询已有视频任务、尝试下载成品时，看到 `io_error: Could not publish the download file`，工作区没有 mp4。没有重新提交生成任务。
- 最后一次成功取得的供应商状态为 `RUNNING`；后续查询被同一 I/O 错误挡住。**视频的当前供应商终态未知**，不能把本地下载错误当成生成成功或失败。

## 证据与边界

- `adb logcat -d --pid` 可读，但当前缓冲区只保留近期 IM 心跳/GC，没有当时的下载记录。Release 包 `run-as` 报 `package not debuggable`，不能读取应用私有诊断文件。
- 报错文本只在 `MaiAgent/AgentTools/MaiDownloadFileTool.cpp` 的 `MaiDownloadSink::publish` 失败后产生。代码到达该分支前已经完成 HTTP 2xx 传输、临时文件写入与 `fsync`，并创建目标目录；旧 Android/POSIX 分支用 `link(temp, destination)` 再 `unlink(temp)` 发布。
- OPPO 的 `/data` 是 F2FS，可用空间约 66 GB。对同一分区 `/data/local/tmp` 的可清理探针，`link` 返回 `Permission denied`；用 NDK arm64 编译的 `renameat2(RENAME_NOREPLACE)` 探针发布成功，目标已存在时返回 `EEXIST=17` 且旧内容不变。探针文件和程序均已删除。
- 上述探针运行在 ADB shell 的 SELinux 上下文，**不能直接证明 App 私有目录的 errno 相同**。旧版工具未记录系统 errno，因此现场只能定位到本地发布步骤，不能把具体权限策略写成已证实的根因。

## 修复与验证

- 修复提交：`d0dbf238bf885c067c2b692e9fc15b323dd35f14`。Android 改用原子、无覆盖的 `renameat2(RENAME_NOREPLACE)` 发布；不支持该操作的旧内核/文件系统才回退原硬链接流程。Windows/Mac 正常路径不变。
- 发布失败的工具结果新增 `stage=publish` 和数值 `os_error`，不暴露签名 URL、文件路径或账号凭据。
- Android NDK arm64 与 armv7（API 26）源码语法编译通过；Windows `MaiDownloadFileTests` 1/1 通过，仓库行宽检查通过。OPPO 的同分区系统调用探针通过无覆盖与目标文件保护检查。
- 待完成：在 Android App 私有工作区以本地无付费 HTTP fixture 跑 `curl_download` 完整链路，并在视频任务有明确终态后安装新签名包做真机验收。当前 OPPO 保持 2.2.1，不能宣称产品故障已经实机修复。
