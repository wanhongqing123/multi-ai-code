# Android 视频下载在发布文件时失败（2026-10-18）

## 现场与复现

- 平台：OPPO PKD130，Android 15，MaiChat 已发布签名包 2.2.1 (221)。
- 用户在创作中心查询已有视频任务、尝试下载成品时，看到 `io_error: Could not publish the download file`，工作区没有 mp4。没有重新提交生成任务。
- 界面最后一次展示的状态为 `RUNNING`。Seedance `continue` 代码仅在当次查询得到 `succeeded` 后调用下载工具；因此这次进入下载分支表明当次查询已取得成功状态，但**当前供应商状态仍未独立查询**，也未拿到本地成片。

## 证据与边界

- `adb logcat -d --pid` 可读，但当前缓冲区只保留近期 IM 心跳/GC，没有当时的下载记录。Release 包 `run-as` 报 `package not debuggable`，不能读取应用私有诊断文件。
- 报错文本只在 `MaiAgent/AgentTools/MaiDownloadFileTool.cpp` 的 `MaiDownloadSink::publish` 失败后产生。代码到达该分支前已经完成 HTTP 2xx 传输、临时文件写入与 `fsync`，并创建目标目录；旧 Android/POSIX 分支用 `link(temp, destination)` 再 `unlink(temp)` 发布。
- 通用 `MaiFileSystem::publishNewFile` 的旧 POSIX 实现也使用 `link`/`unlink`，GLM 媒体、其他创作媒体和文件工具会走这条路径；只改下载 sink 会遗漏这些 Android 发布场景。
- OPPO 的 `/data` 是 F2FS，可用空间约 66 GB。对同一分区 `/data/local/tmp` 的可清理探针，`link` 返回 `Permission denied`；用 NDK arm64 编译的 `renameat2(RENAME_NOREPLACE)` 探针发布成功，目标已存在时返回 `EEXIST=17` 且旧内容不变。探针文件和程序均已删除。
- 上述探针运行在 ADB shell 的 SELinux 上下文，**不能直接证明 App 私有目录的 errno 相同**。旧版工具未记录系统 errno，因此现场只能定位到本地发布步骤，不能把具体权限策略写成已证实的根因。

## 修复与验证

- 下载 sink 修复提交：`d0dbf238bf885c067c2b692e9fc15b323dd35f14`；本记录所属后续提交同时修正通用 `MaiFileSystem::publishNewFile`。两处 Android 路径改用原子、无覆盖的 `renameat2(RENAME_NOREPLACE)`，不支持该操作的旧内核/文件系统才回退原硬链接流程。Windows/Mac 正常路径不变。
- 发布失败的工具结果新增 `stage=publish` 和数值 `os_error`，不暴露签名 URL、文件路径或账号凭据。
- Android NDK arm64 与 armv7（API 26）的两处源码语法编译通过；Windows `MaiDownloadFileTests` 与 `MaiFilePathTests` 各 1/1 通过，仓库行宽检查通过。OPPO 的同分区系统调用探针通过无覆盖与目标文件保护检查。
- Android App 私有目录验证提交 `d35d62cd8827f82d93d3ae3e3aa4e2b847fccacd`：在 Mac ARM Android 模拟器的 MaiChat Debug App 中，用仅绑定 `127.0.0.1` 的无付费 HTTP fixture 测试 `curl_download`。首次发布字节正确；再次下载同名文件返回 `file_exists` 且原文件保持不变。相同 App 私有目录中，通用 `MaiFileSystem::publishNewFile` 新建成功、目标已存在时拒绝覆盖且源文件保留。同进程 instrumentation 2/2 通过，测试文件已清理。路径比较使用 canonical path，因为 Android 的 `/data/user/0` 与 `/data/data` 是同一私有目录的别名。
- 从 `d35d62cd` 构建的 Android 签名 Release 2.2.2（222）通过；测试 JNI 入口只在 Debug 编译，经 APK 内 arm64 原生库符号核对未进入 Release。Mac `MaiDownloadFileTests` 与 `MaiFilePathTests` 2/2、Desktop 隔离 `--smoke` 通过。
- 待完成：OPPO Release App 私有 F2FS 目录的实际下载与供应商任务终态尚未验证。当前 OPPO 保持 2.2.1，不能宣称其用户现场已经修复，也不能凭本地发布错误判断视频生成失败。
