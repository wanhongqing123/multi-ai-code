# Desktop 聊天 Markdown 真实换行被折叠（2026-10-29）

## 现场与复现

- 用户截图 `1793275765-2149791876.png`：创作中心助手回复中，建议 A1 至 A13 挤在同一个长段落，iOS 同类内容分行显示正常。
- 现装 Mac 为此前 `3446aefa` 的 Desktop。协作者只读核对对应消息字段：5562 个字符、111 个 LF、0 个 CR；A1 至 A13 各出现一次，位于第 37、39 至 61 行，每项实际两行。各项不是 Markdown 列表，也没有空白段落分隔，属于同一段落里的软换行。原文没有丢失换行。
- 此文不保存用户审查正文或完整数据库记录。独立复现使用同形脱敏数据：`A1 审查项\n修复建议一\nA2 审查项\n修复建议二`，扩展到 A13，并附加原有列表、表格、代码块。
- 修复源码基点：`965179b5f54158aeae452de6eb0bcd084a7098fd` 加本轮未提交的 Desktop Markdown 修改；测试平台 macOS 27.2、Qt 5.15.18、Release。

## 根因证据与两层修复

1. 创作中心正式路径为 `AgentChatPanel → MarkdownView → MarkdownDocument → MarkdownLayout` 自绘，并非旧的 `MarkdownRenderer` HTML 路径。只修 HTML 不会修复用户这张截图。
2. `MarkdownDocument::onText` 原先把 `MD_TEXT_SOFTBR` 转为空格；HTML 渲染也按 CommonMark 默认折叠软换行，且旧测试明确要求这种行为。新用例在 Document、HTML、实际 Layout 三层均失败，见 `/private/tmp/maichat-desktop-markdown-softbreak-red.log`。
3. 两个 Desktop 解析入口启用 md4c 官方 `MD_FLAG_HARD_SOFT_BREAKS`，由解析器把聊天软换行作为既有硬换行处理。没有替换 Markdown 全文，因此代码块、表格、列表、转义与链接仍由原解析器识别。
4. 仅解析层修复后，Document 与 HTML 用例通过，实际 Layout 高度断言仍失败，见 `/private/tmp/maichat-desktop-markdown-softbreak-green.log`。`QTextLayout` 是单段布局器，段内强制换行需使用 U+2028；直接传 LF 仍不能保证实际断行。
5. `MarkdownLayout::addLaidOutText` 仅在交给 `QTextLayout` 的布局副本中将 LF 等长替换为 U+2028。复制文本、原 Markdown、格式区间及链接的 UTF-16 偏移保持原样。现有代码块逐行绘制不受影响。

## 验证与画面

- 同一个独立 `MarkdownView` Qt 程序、相同 1280×1120 尺寸及脱敏样本：修前 `/private/tmp/maichat-softbreak-before.png` 实际复现 A1 至 A13 连成一段；修后 `/private/tmp/maichat-softbreak-after.png` 每项标题和建议分别显示，13 项均分行可读。执行者已亲看两张原图，附带列表、表格及两行代码保持正确结构。
- 截图由隔离 Qt/offscreen 测试窗口生成，不是用户真实会话，也没有启动或截取现装 App。视觉程序保存在 `/private/tmp/maichat-softbreak-visual.cpp`。
- 最终 `markdown_document_test`、`markdown_layout_test`、`markdown_renderer_test` 三项 CTest 3/3 PASS，5.83 秒。日志 `/private/tmp/maichat-desktop-markdown-softbreak-verified.log`。
- 覆盖真实软换行/已有硬换行、LF/CRLF/CR、A1/A2 同段数据、实际可见高度、复制后仍为 LF 而不泄漏布局 U+2028；原有代码、表格、列表、引用、提示框、链接安全与预览测试同时通过。
- scoped `git diff --check` 通过。未编辑协作者负责的 `MarkdownView` 或子 Agent 卡片文件。

## 交付边界

本轮仅修改 Desktop，未修改 iOS、PDF 生成、md4c 上游源码、超时或模型重试策略。没有触发真实模型请求，没有替换现装 App，没有操作生产服务。Windows 和最终统一打包产物由主执行者另行验收；本文不宣称这些产物已经更新。修复提交号待主执行者统一提交后补充。
