# Android Agent 拨号工具缺失与重复确认

- 时间与平台：2026-10-25，Windows 源码构建，OPPO PKD130 / Android 15 真机。
- 用户现象：希望 Agent 在已获系统授权后按明确指令拨号，不要每次再由 MaiChat 确认。
- 修复前提交：`8ac971d3104ae447f965e8fb6ea0c0d479e4f248`。
- 修复提交：`acbdbff78e83f02d21d4259e2b4bfd9ee9ba43d9`；真机安全拨号
  捕获补充：`fb417263a70cbeaadfd24db7b29ecfa9e606362b`。

## 复现步骤

在修复前 Android 客户端授予通讯录权限，然后要求创作中心 Agent 给联系人拨号。
用户现场报告 Agent 不能完成拨号。源码核对的确定性复现是 Android 条件编译下
工具注册表没有 `mobile_dial_phone`，Java 宿主也没有对应分发；本轮没有用模型
付费调用重演旧版对话。

## 定位证据

`MaiMobileHostTools.cpp` 把 `mobile_dial_phone` 置于 iOS 条件编译中；Android 的
`MaiChatHostTools.execute` 没有拨号分发，Manifest 也没有 `CALL_PHONE`。因此 Android
Agent 无法调用拨号工具。iOS 使用 `tel:` 打开系统电话确认，不存在 MaiChat 自身的
逐次工具审批。Android 的 `READ_CONTACTS` 只能读取联系人，不能替代直接拨号所需的
`CALL_PHONE`。见 [Android Intent 文档](https://developer.android.com/reference/android/content/Intent)
与 [权限文档](https://developer.android.com/reference/android/Manifest.permission)；
[Apple 文档](https://developer.apple.com/library/archive/featuredarticles/iPhoneURLScheme_Reference/PhoneLinks/PhoneLinks.html)
说明 iOS 10.3 起的系统电话确认。

## 改动

Android 注册 `mobile_dial_phone`，仅在用户明确要求呼叫时使用；从联系人姓名拨号
先通过已有只读工具取号码，多号码时由用户选择。Java 普通调用入口与 Agent 工具
共用 `MobileTelephonyTools.dial`。它校验号码和前台状态，首次请求系统
`CALL_PHONE` 权限；获准后调用 `ACTION_CALL`，后续呼叫复用权限，不再弹 MaiChat
逐次审批。拒绝授权时不自动重试。结果只表示系统接受了拨号请求，不表示已接通。
Manifest 将 telephony 硬件标为非必需，以免没有蜂窝功能的设备被误过滤。
iOS 的系统电话确认保持原样。

## 验证与边界

- 单测先因 Android 拨号接口不存在而红，修复后全套 Java 单测 149/149 通过；
  包括首次授权、再次复用、拒绝、无效号码及后台拒绝场景。
- Android arm64-v8a / armeabi-v7a 的共享工具与 JNI 重新编译链接成功；
  Debug、AndroidTest 打包及 Release Java 编译通过。`npm run check:line-width` 与
  `git diff --check` 通过。
- OPPO 隔离测试包上的交互设备用例 1/1 通过：仅获通讯录权限时通话权限仍为
  `granted=false`；首个拨号请求显示 Android 系统授权框，授权后测试捕获到
  `ACTION_CALL` 与正确号码，第二次拨号无需再弹权限框。测试将最终系统拨号接口
  替换为捕获器，没有发起实际电话。
- 最终隔离测试包 SHA-256 为
  `fd4aeed93fd71b9bc2a802d2128b9631d2e62af2e884b256edc477e21ede2bc1`；
  包内 `CALL_PHONE` 和非必需 telephony 声明已核对，OPPO 安装文件哈希一致，
  启动到登录页。独立测试包和测试权限已随卸载清理；用户原 MaiChat APK 哈希
  仍为 `8032b4333c0cb976be95cee695a0b18bc5656502efcd1f4ddf7cf82013f194eb`。
- 本次未拨打真实号码，因此没有验证运营商接通、双卡选择及费用。Windows 侧
  测试包复用了源码未变的 FFmpeg 静态库，尚未做正式签名 Release 全新构建；
  iOS 真机也未在 Windows 侧重测。
