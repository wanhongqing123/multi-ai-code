# iOS 聊天详情页状态栏与导航栏背景不一致（2026-10-24）

- 现场：用户真机 2.2.3(116) 的聊天详情页，状态栏区域偏灰，好友名称、远程桌面按钮和在线绿点所在导航栏为白色。用户提供截图 `/Users/hongqingwan/multi-ai-code/accounts/mac-multi-ai-code/remote-im/images/p_mrst9jeg_9a57d5/144115250159782623-1792805221-4093649086.png`。
- 复现：进入任意好友聊天详情页，观察屏幕最上方状态栏与其下的标题栏。预期两块背景同为白色。
- 原因：`MaiChat/ios/MaiChat/MaiChat/Chat/ChatView.swift` 在选中联系人时把外层背景切为 `RemoteIMStyle.pageBackground.ignoresSafeArea()`；`ChatDetailView` 也把该浅灰背景延伸到顶部安全区，而 `ChatDetailHeader` 使用白色 `panelBackground`。因此状态栏与标题栏取自不同背景。
- 修复：外层导航安全区统一使用白色 `panelBackground`；详情页自己的 `pageBackground` 只绘制内容区域，不再扩展到系统安全区。消息列表页仍为白色。
- 验证：iPhone Air iOS 26.4 模拟器 `testPortraitIMStillNavigatesIntoChat` 1/1 PASS；截图 `/tmp/maichat-chat-detail-color-v117-shots/40E6F847-740F-41EC-BE93-97B21A6EF50D.png` 已目视检查，状态栏和标题栏背景采样均为 RGB(255,255,255)。该改动在工作树中，尚未重新封装或安装到真机；真机当前 116 包仍是旧背景。
