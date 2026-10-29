# MaiChat 已废弃配置的重复启动清理（2026-10-29）

## 请求与核对范围

用户指出 iOS 启动时仍删除旧云服务配置，要求删除无用代码并检查其他位置。检查 iOS、Android、Desktop 的启动、设置与鉴权代码，以及共用移动宿主和网络/工具层；只按实际读写关系认定废弃，不以 `legacy` 或“迁移”字样直接判定无用。没有读取真实用户凭据或修改用户数据。

## 删除项与证据

| 平台 | 删除内容 | 当前依据 |
|---|---|---|
| iOS | `MaiChatApp.init` 中旧 OSS 地址、同步标记、共享令牌和 `cloud-service-setup.json` 的删除 | 五项只剩删除点，无有效业务读写；`1029c9a5d` 已在 10 月 17 日删除手填入口和旧导入路径 |
| iOS | `MaiChatBootstrap.load` 删除旧 IM 签名密钥；`KeychainSecretStore` 的旧默认键名 | `ec5468508` 已在 10 月 12 日改为账号服务领取 IM credential；旧默认键的唯一调用就是删除，当前账号令牌调用均显式传 `account` |
| Desktop | `main` 中三个 `agent/cloudService*` 键删除；`loadAgentModelConfig` 中十个旧地址、wire、API Key、共享 Token 配置键删除 | `cd66d2c91`、`1029c9a5d` 已分别停止旧手填配置和本机共享 Token 的读写；现行路径使用固定供应商地址及账号内存凭据 |
| Android | `clearLegacyModelKeyFiles` 的调用和函数，含旧密钥文件/备份与废弃 Keystore alias | `cd66d2c91`、`1029c9a5d` 已删除对应读写；文件名与 alias 只剩此清理函数 |
| Android | 初始化时删除 settings 的旧 `wanWorkspaceId` 并原子回写 | `3f0562d7c` 已停止该字段持久化；现行 workspace ID 随账号凭据领取并驻留内存 |

同步移除无用导入和两个仅要求清理旧 Token 的 Desktop 测试；回复建议测试不再操作已无业务读取的旧设置键。更新注释说明当前账号凭据路径、Keychain 用途隔离，以及 Desktop 的真实持久配置。保留账号凭据不落盘、草稿与失败状态等现行功能测试。

## 仍有用途的迁移与清理

- iOS、Android 的旧工作区归属标记仍决定读取哪个会话数据库；历史消息迁入 SQLite 及数据库结构升级仍服务已有消息。
- Desktop `LocalMessageDatabase` 补列和消息前缀兼容仍由当前 SQL 和历史消息读取使用。
- 正常退出/会话失效时清除当前账号令牌，以及播放缓存、传输检查点、录音/图片临时文件清理仍属于当前生命周期。
- iOS 的三个账号凭据刷新调用、Android 的账号模型/Seedance/百炼领取和 Desktop 的账号内存凭据来源保持原有实现。

本轮只移除应用中的旧配置扫尾代码；从未执行过扫尾的旧安装，其遗留文件/键不再由这些启动路径主动清除，现行业务也不会读取它们。

## 验证

当前变更基点为 `aaedc2c4b`，修复在本文件所属提交。本轮没有新增对删除语句本身作镜像断言的测试，也没有修改产品版本或替换现装 App。

- iOS：当前 Xcode / iOS Simulator 26.5 增量构建 exit 0。正常 App 用隔离本地账号启动，PID 30237；已查看实际截图与完整 15.65 秒启动录屏的逐段联系图，进入消息页。假腾讯凭据产生已知 70402 提示，不能算作真实 IM 验收；没有触发模型调用。日志 `/private/tmp/maichat-ios-obsolete-cleanup-build.log`，画面证据 `/private/tmp/maichat-ios-obsolete-cleanup-startup*`。
- Desktop：macOS 27.2 / Qt 5.15.18，`maichat` 与 `main_window_layout_test` 增量构建 exit 0。账号凭据内存更新和不落盘、单独 DeepSeek 凭据、失败刷新、回复建议只填草稿、内联模型设置五个现行用例通过，含初始化/收尾 **7 PASS、0 FAIL、0 SKIP，36.031 秒**。隔离测试使用 FakeRemoteIMClient，不使用生产账号。日志 `/private/tmp/maichat-desktop-obsolete-cleanup-build.log`、`maichat-desktop-obsolete-cleanup-tests.log`。
- Android：OpenJDK 17，隔离配置 `:app:compileDebugJavaWithJavac` exit 0，35 秒；仅增量 Java 编译，没有触发 native 构建。`gradlew` 原本没有执行位，使用 `bash gradlew`，未改变文件模式。日志 `/private/tmp/maichat-android-obsolete-cleanup-build.log`。
- 三端源文件中废弃 OSS/IM 键、旧清理函数和旧迁移失败标记已无引用；iOS 两个实际 Keychain 构造均明确指定当前账号刷新令牌。`git diff --check` 通过。
- 未构建 Windows；未重新部署 iPhone 或 Mac，未做 Android 运行时验证。以上构建结果不代表现装 App 已包含本轮源码清理。
