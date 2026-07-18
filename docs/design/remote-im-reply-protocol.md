# Remote IM AICLI 回复协议

## 背景

Remote IM 会把手机端消息注入当前 AICLI，并要求 AICLI 把需要回传到 IM 的内容包在回复标记内。

协议解析由 `electron/remote-im/replyProtocol.ts` 统一负责。Claude、Codex 和 OpenCode 必须共用这层协议解析；不同 AICLI 只允许在输出来源、提交方式和界面显示上做适配。

## 消息提交与显示

- Codex 和 OpenCode 的普通 IM 消息必须通过源码控制桥提交，不能依赖 PTY 模拟输入。
- `text` 是包含回复协议的模型输入，`displayText` 是 AICLI TUI 中展示给用户的文本。
- Codex 通过历史显示覆盖保存 `displayText`，但向核心提交 `text`。
- OpenCode 标准 TUI 把 `text` 保存为 `synthetic` part，把 `displayText` 保存为 `ignored` part：模型只读取前者，TUI 只显示后者。
- Claude 保持现有 PTY 输入和 transcript 读取方式。
- AICLI 回给 Electron 的结构化输出保留完整回复标记；Codex/OpenCode TUI 渲染时只显示标记内正文，且不能在流式输出阶段闪现半截标记。

## 标记格式

旧格式：

```text
<remote-im-reply>
要发回 IM 的内容
</remote-im-reply>
```

带 `replyId` 的格式：

```text
<remote-im-reply id="rim-xxx">
要发回 IM 的内容
</remote-im-reply id="rim-xxx">
```

兼容格式：

```text
<remote-im-reply id="rim-xxx">
要发回 IM 的内容
</remote-im-reply>
```

第三种格式必须支持。原因是模型可能正确输出带 id 的开始标记，但仍输出旧式结束标记。如果开始标记已经匹配当前 `replyId`，旧式结束标记也应闭合当前回复。

## 解析规则

1. 没有指定 `replyId` 时，旧格式 open/close 可正常提取。
2. 指定 `replyId` 时，只接受当前 `replyId` 的 open tag。
3. 当前 `replyId` 的 open tag 已匹配后，允许两种 close tag：
   - `</remote-im-reply id="当前 replyId">`
   - `</remote-im-reply>`
4. 错误 `replyId` 的 close tag 不能闭合当前回复。
5. 未闭合内容保持 pending，不转发到 IM。
6. 旧回复、prompt echo、上一轮残留不能被当前 `replyId` 误收。

## AICLI 边界

共享层：

- `replyId` 生成
- tag 构造
- tag 提取
- pending buffer
- close tag 兼容

Claude 专属：

- 优先从 Claude transcript 读取原始 Markdown。
- transcript 读取仍然调用同一个 `extractRemoteImReplyOutput`。

Codex 专属：

- 过滤 Codex TUI 的启动提示、模型状态栏、输入框建议。
- Codex 噪音过滤不能替代协议解析。

OpenCode 专属：

- 标准 TUI 从已完成的 assistant text part 回传原始 Markdown。
- `--mini` 运行时在 turn 完成后回传原始 assistant text。
- 两种 TUI 都只渲染回复标记内正文，不能修改发送给 Electron 的原始文本。

## 固定测试

协议矩阵测试在：

```text
electron/remote-im/replyProtocol.test.ts
```

必须覆盖：

- open 无 id + close 无 id
- open 有 id + close 有同 id
- open 有 id + close 无 id
- open 有 id + close 错 id
- old reply + current reply 混排
- prompt echo 不被识别为回复

Claude transcript 测试在：

```text
electron/remote-im/claudeTranscript.test.ts
```

必须覆盖：

- transcript 原始 Markdown 提取
- 当前 `replyId` 匹配
- open 有 id + close 无 id

转发层 replay 测试在：

```text
electron/remote-im/outputForwarding.test.ts
```

必须覆盖：

- Claude id-open + legacy-close 能发出 IM 消息
- Codex TUI 噪音不会进入 IM 消息

真实事故 fixture 放在：

```text
electron/remote-im/fixtures/reply-protocol/
```

线上出现过的协议断转发或噪音片段，要先固化为 fixture，再修代码。

## 修改门禁

凡是修改以下文件，必须运行 Remote IM 专用测试：

```text
electron/remote-im/replyProtocol.ts
electron/remote-im/outputForwarding.ts
electron/remote-im/claudeTranscript.ts
electron/remote-im/outputSanitizer.ts
electron/remote-im/ipc.ts
src/remote-im/**
electron/preload.remoteIm.test.ts
```

命令：

```bash
npm run test:remote-im
npm run typecheck
```

如果新增了线上事故样本，必须同时新增 fixture 和对应 replay 测试。

CI 自动门禁：

```text
.github/workflows/remote-im-tests.yml
```

该 workflow 会在 `main` 的 push / pull request 中，只要命中 Remote IM 相关路径，就自动运行 `npm run test:remote-im` 和 `npm run typecheck`。
