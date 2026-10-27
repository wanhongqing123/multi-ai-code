# Windows 跨设备会话附件被误判为无效路径

- 复现：2026-10-27，Windows 10 / VS2019 / Qt 5.15.2，源码 `3459e49c`。运行 `desktop_session_sync_host_test` 的 `resolvedSessionFileStaysInsideItsOwnerWorkspace`：工作区内 `.maichat/session-attachments/report.txt` 已创建且可读，但 `applyCommand` 返回“会话附件路径无效”。单例测试为 2 通过、1 失败；完整 Host 有 1 项失败。
- 根因：`DesktopSessionSyncHost.cpp` 用 `QDir::separator()` 给附件根目录加尾部分隔符；Windows 上它是反斜杠，而 `QFileInfo::canonicalFilePath()` 使用 Qt 路径形式。`startsWith` 因分隔符不一致拒绝了合法文件。
- 修复：将附件根目录与附件文件的路径都转为 Qt 正斜杠形式，保留根目录末尾 `/` 作为路径段边界。仍以规范路径检查归属，工作区外文件继续被拒绝。
- 验证：同一 Windows 单例由 2 通过、1 失败转为 3/3 通过；完整 Host 24/24 通过。重链 Desktop 主程序后，创作中心、布局和云盘 Agent 本地 TLS 相关 CTest 3/3 通过。独立 `maichat.exe --smoke` 退出 0，日志显示 Windows 构建路径、版本 2.2.29、Qt 5.15.2 和 OpenSSL 3.5.9。此前同批源码的共享客户端附件本地 TLS 1/1 通过（53.15 秒）。
- 边界：生产附件服务与 OSS RAM 权限尚未部署，未测试真实账号跨设备上传下载；未安装或替换用户现用 App。
