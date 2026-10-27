# macOS Desktop 账号证书资源链接失败（2026-10-28）

- 平台：macOS 26.4.1 arm64、Qt 5.15.18，源码基线 `d87f330ff`。
- 复现：`cmake --build MaiChat/desktop/build-screenshot-mac --target maichat -j4`。
- 红态：编译完成后链接报 `Undefined symbols for architecture arm64: (anonymous namespace)::qInitResources_AuthCertificates()`，无法生成正常 App。
- 根因：Windows 代理路径重构把 `performWithRoute` 移入匿名命名空间；其中的 `Q_INIT_RESOURCE(AuthCertificates)` 随之声明了命名空间内的资源初始化符号，而 Qt 的 `.qrc` 生成符号位于全局命名空间。
- 修复：在全局命名空间的 `initializeAuthCertificates()` 中调用 `Q_INIT_RESOURCE`，`performWithRoute` 调用该函数；Windows 分支不变。
- 绿态：同一 Mac Release `maichat` 目标链接成功；`build-screenshot-mac/MaiChat.app/Contents/MacOS/MaiChat --smoke` 退出码 0，日志版本 2.2.30、Qt 5.15.18，路径指向本次构建产物。
- 边界：这验证了 Mac 链接和启动，不代表账号网络请求、安装包或 Windows 构建已经验收。
