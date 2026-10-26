# MaiChat Desktop 工作目录切换新建会话与重复发送图片

- 报告时间与平台：2026-10-23，macOS 26.4。用户反馈 Desktop 创作中心在既有会话中选择工作目录会新增会话；随后在普通 IM 聊天中反馈，首条图文发送后，下一条纯文字又带上同一张图。
- 源码基线：`8ac971d3104ae447f965e8fb6ea0c0d479e4f248`，工作树已有其他未提交修改。本次没有提交或替换正在运行的 App。现场进程为 `/Applications/MaiChat.app/Contents/MacOS/MaiChat`，安装包版本 2.2.11，可执行文件 SHA-256 `b0492dd69189f518200af01ba0ba6ac708177a98d874250229efbfc3ba82087a`。

## 工作目录切换

- 复现：打开已有创作中心会话，在右上角菜单中选择另一工作目录。原代码 `AgentChatPanel::openSessionInDirectory()` 总是调用 `AgentController::createSession()`；测试中的会话 ID 从 `ses_01M5KCVMDM0004ZCQF393` 变为 `ses_01M5KCVMAX000ZS3Z72NQ`（具体 ID 每次不同）。
- 根因：界面把目录选择和新建会话绑定；底层 `MaiUpdateSession.directory` 已支持原位修改，但 Desktop 没有调用。
- 修复：新建对话入口仍新建会话；已有会话选择目录时更新原会话的目录并刷新界面，保留 ID 和消息；运行中任务不允许切换，以免一次任务跨两个目录。目录选择的可访问名称与提示文字同步更新。
- 回归：`chosenWorkspaceKeepsCurrentSession` 修改后先红后绿；最终定向测试 6/6 通过，包含会话 ID、历史消息、未发送草稿保留，以及新建会话仍使用默认工作区、菜单可见性和运行中拒绝切换。
- 边界：历史消息本身仍在数据库。历史工具结果若只保存了相对于旧目录的文件路径，切换后并未自动移动文件，重新打开旧媒体可能需要原目录。本次不迁移或删除用户文件。

## IM 图片重复发送

- 现场只读核对：`~/Library/Application Support/MaiChat/Desktop IM/RemoteIMHistory/whq-iphone/messages.db` 中，目标聊天于 11:25:50 和 11:27:33 的两条本人发送记录均为 `has_image=1`，两条 `image_path` 完全相同；11:32:40 后续纯文字测试为 `has_image=0`。未读取或记录消息正文。证明图片确实进入第二条本地发送记录，并非单纯重复渲染。
- 最小失败用例：普通 IM 输入区插入图片和文字，发送过程中同步触发会话状态变更与联系人切换，再回到原联系人。原代码先调用 `app_.sendImage()`，发送引发的刷新会把未清空的图片保存为草稿；直到全部发送之后才 `messageEditor_->clear()`，此时清空的可能已是别的联系人的输入框。测试 `sentImageDoesNotReturnAsDraftDuringConversationChange` 在旧代码中失败，旧图留在原联系人输入框。
- 修复：先快照收件人、文字、附件、引用和配文顺序，清空这一条输入与引用，再逐项发送到快照中的收件人。这样同步刷新无法把已发图片重新存成草稿；多附件发送中途切换联系人也不会把剩余附件发错人。
- 回归：相同用例由红变绿；普通文字、图片、跨联系人草稿、引用与多行文本的定向测试 9/9 通过。测试窗口截图 `/tmp/maichat-repeat-image-after.png` 已人工查看：第一条为图片加配文，第二条只有文字。
- 边界：测试使用 Fake IM 客户端复现 UI 重入，没有向真实联系人发送测试消息。现场安装版仍为旧版本；本次源码修复未进入该进程。

## 构建与运行边界

- Mac Release 源码构建 `maichat` 成功；独立构建产物 `MaiChat/desktop/build-release/bin/MaiChat.app/Contents/MacOS/MaiChat --smoke` 退出 0，日志版本 2.2.13，OpenSSL 可用。
- Windows 未构建。本次未停止或替换用户正在运行的 2.2.11；旧安装版仍会表现为旧行为，直到新版本安装并运行。
