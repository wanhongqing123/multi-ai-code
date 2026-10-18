# Android 联系人行露出删除背景（2026-10-19）

## 现场与复现

- 平台：Mac 上的 arm64、16 KB 页 Android 模拟器 `emulator-5560`；MaiChat 2.2.2 隔离 Debug 包 `com.kongshang.maichat.isolated`。
- 源码：`b2eef86e`（账号好友产品代码为 `a848a72b1`）。用无手机号临时账号登录，完成好友申请与同意，打开“通讯录”。联系人行尚未滑动时，右上和右下圆角露出红色“删除”背景，见[修前截图](assets/maichat-android-swipe-corner-before-20261019.png)。
- 左滑同一行能正常露出“删除”，故红色角并非待同步状态或账号服务返回值。

## 原因与修复

`SwipeActionRow` 把红色操作按钮常驻放在圆角内容下层。联系人卡片的圆角区域透明，未滑动时也能透出下层红色。修复提交 `2c724584` 在可平移的内容前景里加一层与列表底色一致的遮罩，盖住透明角；前景左移后，右侧空出的操作区域仍显示红色按钮。消息列表共用此行组件，也采用同一修正。

## 验证与边界

- 从 `2c724584` 源码执行 `:app:assembleDebug -PmaichatIsolatedDebug=true` 成功，安装到同一模拟器；包 SHA-256 为 `1f5ec8febaf7f56dc41759d9141e70b7ea57c21cb2d894db4dfa4a097ab44acf`。
- 同一好友列表行未滑动时圆角不再露红，见[修后截图](assets/maichat-android-swipe-corner-after-20261019.png)；左滑仍显示“删除”，点击并确认后该行消失。
- 测试账号退出并全部停用，生产库只读核对活跃 QA 账号、QA 好友边、IM 同步队列均为 0，数据库完整性 `ok`。`npm run check:line-width` 和 `git diff --check` 通过。
- 此次是模拟器 Debug 界面验收；OPPO 真机和正式签名包尚未验证。该模拟器对包内若干第三方库另有 16 KB ELF 对齐兼容提示，与本次红角现象无关。
