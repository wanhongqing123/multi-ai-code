# Desktop 子 Agent 卡片标题裁切、摘要只剩标点（2026-10-29）

## 现场与范围

- 用户截图 `144115250159782623-1793275489-2149791857.png`：四张子任务卡只显示“子 完”和标点，周围父会话正文正常。
- 运行产物仍是 `/Applications/MaiChat.app` 2.2.31 / `3446aefaa14315007b206f6f71030b0d732bfd74`，与同日模型流排障中只读核对的产物一致。该版本与修复前当前源码的 `SubAgentCard` 实现相同。
- 当前会话数据库只读核对：四个子会话 `logic-func-reviewer`、`code-safety-reviewer`、`code-spec-reviewer`、`arch-reviewer` 均存在已完成的 assistant 记录，正文分别累计 346、566、321、668 字符。父会话四次 `wait_agent` 回执也已保存，输出分别为 347、571、322、670 字符。因此结果不只有截图里的单字/标点；本次不判断业务审查结果的质量。

## 根因与最小复现

使用真实 `AgentChatPanel`、内存数据库和无网络的 `SilentModel` 创建子会话，再发布安全测试文字的流式事件：

1. `subAgentCardFor` 先把空标签卡片加入 `MarkdownView`，之后 `noteOtherSession` 才填写标题、状态和摘要。卡片采用横向 `Maximum` 策略，宽度来自 `sizeHint`；`MarkdownView::eventFilter` 却只比较高度，文本填入后同高异宽的布局请求被忽略。
2. 修复前真实几何为卡片 `45×50`、期望 `227×50`；标题、状态标签各只有 7px 宽，标签实际文本仍完整，故出现“子 完”。
3. `textDelta` 回调把本次增量直接交给 `setLatest` 覆盖摘要。连续输入“代码审查已完成，发现两个需要核实的问题”和“。”后，标签实际文本就只剩“。”，与用户截图一致。

## 修复与验证

- `MarkdownView` 对嵌入组件同时比较所需宽度和高度，仅真正变化时重排，保留防止 `Resize` 递归的边界。
- 子任务摘要按 part ID 累计流式增量；新正文片段清除上一片段，最多保留 4096 个 UTF-16 单元尾部，仍只显示最多 60 字的最后一行。完成状态、父会话消息隔离和原卡片容器保持原逻辑。
- 同一独立 Qt 用例修复前 2 PASS / 1 FAIL，修复后定向五个 panel 用例 7/7 PASS（含初始化/收尾，2.327s），覆盖标题/状态几何、最终标点、片段切换及跨会话消息/标题隔离。`markdown_view_test` 整套 12/12 PASS（1.165s），覆盖滚动、嵌入组件高度变化、批量历史布局等。
- 已亲看对照截图：`/private/tmp/maichat-sub-agent-card-before.png` 与 `/private/tmp/maichat-sub-agent-card-after.png`。后图完整显示任务名、完成状态和整行摘要。
- 日志：`/private/tmp/maichat-sub-agent-card-before.log`、`/private/tmp/maichat-sub-agent-card-final.log`、`/private/tmp/maichat-sub-agent-markdown-view.log`；构建为 macOS ARM64 / Qt 5.15.18 Release，测试用 offscreen 平台绘制。

## 未验证边界

未触发真实模型、未重启或替换现装 App，也未做 Windows 构建。用户当前运行的 2.2.31 尚未包含本修复。另一个 A1–A13 文本换行问题属于 Markdown 解析链，单独处理，不与本卡片故障混记。

## 统一构建记录

修复提交 `6e97705d4d2bafd203a523dae3e8d4ab1a1a6388`，已推送 main。统一 Desktop **2.2.32** staging 完成签名和裸 PATH `--smoke`（exit 0）；主程序 SHA256 `fcda1b0186456ab12c6703877cc95e045df815559945d56d816c265eb7e2644d`。主执行者已查看上述修后截图。现装 2.2.31 尚未替换，具体产物与用户确认状态见 `maichat-model-stream-protocol-liveness-20261029.md` 的完成记录。
