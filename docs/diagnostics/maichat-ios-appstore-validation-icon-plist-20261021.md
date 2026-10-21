# iOS App Store 验证失败：图标透明与 Bundle 类型缺失（2026-10-21）

## 现场与复现

- 平台：macOS 26.4.1、Xcode 26.6。Xcode Organizer 对 2026-10-21 14:18 的
  `MaiChat 2.2.3 (94)` 归档执行 `Validate App`。
- 归档 Team 为 `VLFG56JT33`，Bundle ID 为 `com.kongshang.maichat`；归档由
  `Apple Development: xiaojing he` 签名。源代码基线提交为 `1514433e`；
  归档未嵌入 Git 提交号，不能仅凭时间证明其精确源码状态。
- App Store Connect 返回 `90717 Invalid large app icon`，说明主图标不得含透明度
  或 alpha；同时返回 `90183 Invalid Bundle OS Type code`，要求主 App 的
  `CFBundlePackageType` 为 `APPL`。构建、签名与本地导出此前均成功，错误在
  苹果远端验证阶段出现。

## 根因证据

- `Assets.xcassets/AppIcon.appiconset/AppIcon.png` 为 1024×1024 RGBA；
  45,515 个像素 alpha 不等于 255，四角完全透明。
- 仓库 `MaiChat/Info.plist` 与旧归档内的最终 `Info.plist` 都没有
  `CFBundlePackageType`。Apple 对应用要求值为 `APPL`。

## 修复与验证

- 把主图标的透明角铺在原图蓝色渐变上，输出无 alpha 的 RGB PNG；原图所有
  不透明像素的 RGB 保持逐像素一致。
- 在 iOS 主 App `Info.plist` 明确设置 `CFBundlePackageType=APPL`；构建号从
  94 提至 95，使修复后的归档与旧验证失败的归档可区分。
- 静态检查：图标仍为 1024×1024，`sips` 报告 `hasAlpha: no`；源 plist 与
  Xcode 工程的 `plutil -lint` 均通过。
- 独立新归档 `/tmp/maichat-appstore-2.2.3-95.xcarchive` 完成，归档内
  `CFBundleIdentifier=com.kongshang.maichat`、版本 `2.2.3 (95)`、
  `CFBundlePackageType=APPL`，隐私清单与 AppIcon 资源均存在。
- 从新归档导出 App Store IPA 成功。IPA 内的描述文件为
  `iOS Team Store Provisioning Profile: com.kongshang.maichat`，
  `get-task-allow=false`；签名为 `Apple Distribution: xiaojing he (VLFG56JT33)`，
  `codesign --verify --deep --strict` 通过。
- 对新归档执行 Xcode `method=validation` 的苹果上传验证，退出码为 0，日志
  `Validated MaiChat`、`EXPORT SUCCEEDED`；旧错误码 `90717` 与 `90183`
  未再出现。验证过程中有若干第三方框架缺 dSYM 的符号上传警告，未阻止验证。
- 新归档另存于 Xcode Organizer 的 `2026-10-21/MaiChat 10-21-26, 2.42 PM
  build95.xcarchive`，旧 `94` 归档未覆盖。此处验证的是二进制，未上传 App Store
  Connect 构建记录。

## 未验证边界

- 这次只针对苹果报告的两条二进制验证错误。App Store 内容审核、后端上线
  与审核资料仍需各自验收；本修复不代表 App 已提交审核。Xcode 导出和验证日志
  另有 `No provider associated with App Store Connect user` 配置查询警告，
  未阻止本次发行导出与验证，不能据此证明正式上传接口也已打通。
