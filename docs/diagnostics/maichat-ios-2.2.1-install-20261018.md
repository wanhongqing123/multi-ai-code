# MaiChat iOS 2.2.1 当前源码安装记录

- 时间：2026-10-18，源码基线 `1e05ab27f2d7efc3848388e0822cf6602da8da46`。构建工作树另有事先存在、未提交的 iOS Xcode 工程文件和 Info.plist 改动；本次未改写或提交它们，因此产物应按“该基线加现有本地工程配置”追溯。Xcode 26.6；连接设备 iPhone 14 Pro Max `CE84873B-0ADF-55F6-9E54-284B6860F904`。
- 原设备显示 `com.kongshang.maichat` 2.2.1 (89)，仅凭相同版本号无法确认是否包含后续 Seedance/Seedream 工具下架等共享内核修改，因此从当前源码重建。
- 模拟器：Release 首次构建的 OpenSSL 子构建拒绝组合架构 `arm64;x86_64`；以 `ARCHS=arm64 ONLY_ACTIVE_ARCH=YES` 重跑成功。把产物安装到已启动的 iPhone 17 Pro 模拟器，正常启动 App；截图 `/tmp/mai-ios-current-normal.png` 显示登录页。模拟器可执行文件 SHA-256 `8c697a08df75925715f95e652b713e51d07ddcfa6d4bcaad3c4b4f712e7d550e`。
- 实机：Release arm64 构建成功；`codesign --verify --deep --strict` 通过，Bundle ID `com.kongshang.maichat`，Team ID `VLFG56JT33`，版本 2.2.1 (89)。本地可执行文件 SHA-256 `ff306c7e6a9a0af3465db178e7934031e5394387a0bbb4f13e5df5c3fbcdeb81`。
- `devicectl device install app` 成功覆盖安装到 `/private/var/containers/Bundle/Application/4A5DF40E-2B5F-45D7-BE3D-88CC3E9F7345/MaiChat.app/`；安装后设备回读仍显示 2.2.1 (89)。本次没有卸载 App、清除应用数据、调用付费模型或修改后台服务。
- 实机启动验收尚未完成：`devicectl device process launch` 被 SpringBoard 以 `Locked` 拒绝，提示设备未解锁。已向用户请求解锁并保持连接；解锁后需重新启动并检查正常页面与相关旧功能。
