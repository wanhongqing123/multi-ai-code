# iOS IM 历史消息快速滑动后持续滚动（2026-10-26）

## 现场与复现

- 用户在 iPhone 的 IM 会话中用力向下滑动查看旧消息，松手后列表继续向更早的消息移动，似乎一直停不下来。
- 排障基线为 `47812c0d4f5c5ea7561aa72379558f75ba838908` 加当时未提交的产品改动；测试设备为 iPhone 17e 模拟器、iOS 26.4，Debug `MaiChat`。未用用户的 IM 数据做破坏性操作。
- 在 `--ai-ui-test --im-large-history-ui-test` 下，用仅限测试的 SQLite 历史数据复现：同一个会话存入 2544 条消息，进入时只读取最新 20 条。持续向下滑到首次触发更早一页后停止操作，等待 2 秒。
- 修复前，界面已加载数量从 20 越过首个 40 后自行增加到 **500**；`testDatabaseHistoryLoadsOnePageAtTop` 因要求最多 40 条而失败。修复后同一步骤稳定保持 **40**，测试通过。

## 原因与改动

`MessageHistoryStack` 使用立即排版的 `VStack`。每次 SQLite 旧页合并后，新首行的 `.onAppear` 又调用 `loadEarlierMessagesIfNeeded()`；下一页插入时重复该过程，形成无须新手势的翻页链。内容高度持续增长，也放大了松手后的滚动感。

移除了首行 `.onAppear` 中的自动翻页。保留顶部拖动触发和“加载更早消息”按钮；手指松开后惯性滚动才到达顶部时，由滚动视口观察器检查边界并请求一页。两种顶部触发复用同一手势门闩 `requestedTopForGesture`，一次手势最多请求一页。保留系统原有的减速率，避免为了抑制症状而让正常翻历史需要更多滑动。

## 验证与边界

- SQLite UI 回归：修复前 **500**（失败）→ 修复后 **40**（通过），均为首次顶部翻页后等待 2 秒。
- 最终代码在 iPhone 17e / iOS 26.4 模拟器上跑过三项有针对性的 UI 测试，均通过：SQLite 每次手势只读一页、原有 2544 条内存历史首屏与八次滑动翻页、打开 180 条历史时的快速滑动。
- 逐张查看快速滑动截图：松手后与 3.5 秒后的可见消息位置相同；会话消息按顺序显示，未见重复行或空白。测试数据在每次运行前清除，仅针对合成账号 `im-layout-test`，避免上次的 SQLite 数据与本轮新 UUID 混合。
- 已构建开发签名 Release `MaiChat 2.2.3 (127)`，Bundle ID `com.kongshang.maichat`，Apple Development / Team `VLFG56JT33`。IPA 位于 `MaiChat/ios/MaiChat/dist-im-history-v127/MaiChat-iOS-2.2.3-127-im-history-dev.ipa`，263,161,711 字节，SHA-256 `e9370732e8bd8873cff30a63d38c9ea40cdbabe2a81c97de7386557a4d039db7`；严格深度签名校验和 ZIP 完整性检查通过。
- 同日按用户要求，把上述签名 127 App 原位安装到 iPhone 14 Pro Max。设备回读只有一个 `com.kongshang.maichat`，版本从 126 升到 127；App 数据根目录安装前后均为 3 项且名称集合一致；正常启动后查到 MaiChat 进程。未安装第二个 Bundle ID。
- 真实手指的强甩手感尚待用户在 127 上反馈；模拟器手势速度可能不同。安装和启动成功只证明包已更新，不把它当作真机滚动体验验收。
