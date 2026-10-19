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

## Android Java/JNI 文件发布调整（同日）

- 用户要求 Android 的文件发布操作由 Java 系统文件接口实现，并通过 JNI 供 MaiAgent 的 C++ 文件接口调用。临时下载文件与最终文件都在当前账号的 App 私有工作区内；普通写入已成功，失败发生在旧硬链接发布步骤。
- 调整只覆盖 Android 的“完整临时文件 → 新目标文件”发布：`AndroidFileOperations.publishNewFile` 调用 `Files.move(source, target)`，JNI 在原生工作线程上附着 JVM 并调用该方法。`MaiFileSystem::publishNewFile` 和 `curl_download` 共用此回调；iOS/macOS/Windows 的文件操作分支未改。
- Java 方法串行化本进程经桥接进入的发布调用，目标已存在时返回明确错误并保留源文件；严格 UTF-8 解码避免默默改写路径。Java 的无选项 `Files.move` 文档不保证与原 `RENAME_NOREPLACE` 同等的原子无覆盖语义：若另一进程或绕过此桥接的同进程写入者恰在存在性检查后创建目标，仍可能被重命名覆盖。因此此实现限定 MaiChat 单进程 App 私有工作区，不作为外部共享目录或任意并发写入者的通用替代。
- 新 Java 单测先因缺少实现而编译失败，添加实现后通过首次发布、重复目标、非法字节路径、Unicode 路径和本进程并发发布；Android 整组本地 Java 单测、Debug/Release Java 编译及 instrumentation Java 编译通过，arm64/armv7 相关三处 C++ 文件的 Debug/Release 语法编译通过。Windows MaiAgent 增量构建、CTest 38/38 和假模型控制台端到端检查通过。Android instrumentation 已补断言，要求实际原生下载和通用发布都进入 Java 方法；此阶段 Windows 尚无完整 Android ONNX/OpenSSL 源构建产物，OPPO 已安装 2.2.1 未替换。后续真机验收见下一节。

## Windows 本机构建与 OPPO 真机隔离验收（2026-10-19）

- Windows 补齐 Git Bash 下的 Android NDK/OpenSSL、Meson/FFmpeg 源构建路径，arm64 与 armv7 的 Debug 和 Release 原生库均从当前源码完成链接。ONNX Runtime 仅复用已发布 2.2.1 APK 中的两个原生库；该 APK SHA-256 为 `c2d0daacb6e6f464fe12ee37abb48e5f447305f9cd0ab92513ff9ecc61f070f4`，对应依赖源码相对当前提交无变化。
- `assembleDebug` 完成，`assembleRelease` 在没有发布密钥的 Windows 上生成未签名的 2.2.2 (222) 双 ABI APK，158791349 B，SHA-256 `96435f418838df40de3f57455195c2b4a478a61af6c9de78b5706bfd91f5a43b`。arm64 Release `libmaichat_agent.so` 含 Java 文件操作类名且导出 `JNI_OnLoad`。
- 另用本机 Debug 证书签了隔离候选：`.cache/android-candidate/MaiChat-Android-v2.2.2-3da73b68-local.apk`，158845288 B，SHA-256 `4bd95418977838bbc0f640ee2cbea0b9ed8f2caf5ac8adfd75321cc18911e320`。证书与 OPPO 已安装 2.2.1 发布证书不同，不能直接覆盖安装并保留数据；此候选尚未安装到原包名。
- 为避免触碰原版数据，以独立包名 `com.kongshang.maichat.isolated` 在 OPPO PKD130 / Android 15 / 4 KB 页面真机安装 Debug 版。同一 App 私有目录中，`DownloadPublishInstrumentedTest` 的本地 HTTP 下载与通用文件发布 2/2 PASS，并断言 C++ 两条路径确实回调 Java；重复目标未覆盖。隔离版冷启动进入手机号登录页。随后卸载测试 App 与测试包、清理临时 UI dump；原版 `com.kongshang.maichat` 仍是 2.2.1 (221)，数据未清除。
- 待完成：取得原发布签名密钥后覆盖安装，或获得用户明确允许清除旧版数据后安装本机签名包；之后才可在原账号会话里复测既有供应商视频任务的真实下载。当前真机本地 fixture 通过不等于旧版 App 已修复，也不证明云任务当前状态。

## OPPO 安装结果（2026-10-19）

- 用户明确允许卸载旧版并清除 MaiChat 本机数据。Windows 从 `cef97b3b7cd7ae18e102c415e9d2383ea6fb8d39` 重建、以本机 Debug 证书签名的优化 Release APK 为 `MaiChat-Android-v2.2.2-cef97b3b-local.apk`，158845288 B，SHA-256 `4bd95418977838bbc0f640ee2cbea0b9ed8f2caf5ac8adfd75321cc18911e320`。包内 versionName=2.2.2、versionCode=222，包含 arm64-v8a 与 armeabi-v7a。
- 安装前设备是 OPPO PKD130 / Android 15 / 4 KB 页面，原包为 2.2.1 (221)。`adb uninstall com.kongshang.maichat` 与新包 `adb install` 均返回 Success；设备包管理器回读 `2.2.2 (222)`。从设备实际安装路径拉回 `base.apk`，SHA-256 与本地候选逐字节一致。冷启动 `Status: ok`，实际截图显示手机号登录页；进程 PID 在启动后保持稳定，同 PID 的近期日志未匹配到 Java/native fatal。
- 安装前已用独立包名在同一 OPPO 上跑 App 私有目录本地 HTTP 与通用发布 instrumentation 2/2 PASS；测试包已卸载。正式 2.2.2 清除了原账号本机数据，用户需重新登录。原供应商视频任务记录无法在此手机上继续复测；没有创建新的付费任务，因此真实供应商视频完成后的下载仍标为未验证。
