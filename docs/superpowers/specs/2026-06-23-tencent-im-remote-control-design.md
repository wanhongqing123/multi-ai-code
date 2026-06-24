# 腾讯 IM 远程控制 AICLI 设计

## 背景

Multi-AI Code 当前通过本机 AICLI 会话完成 AI 开发任务。新需求是让用户可以在手机端发送消息，由 Multi-AI Code 接收消息并转发给当前 AICLI，再把 AICLI 的输出回发到手机端。

第一版定位为个人远程控制，不做团队协作系统。

## 目标

- 手机端通过腾讯 IM 给桌面端 Multi-AI Code 发送文本消息。
- Multi-AI Code 校验消息来源后，把消息发送给当前主 AICLI 会话。
- Multi-AI Code 捕获 AICLI 输出，合并后通过腾讯 IM 回发给手机端。
- 桌面端提供远程 IM 设置界面和运行态会话抽屉。
- 本地保存远程消息记录，便于查看状态和排查问题。

## 非目标

- 不在第一版支持多用户团队协作。
- 不在第一版支持远程创建新 AICLI。
- 不在第一版支持远程切换项目。
- 不在第一版支持图片、文件、语音消息。
- 不在第一版支持群聊。
- 不在第一版直接执行本地系统命令。
- 不把腾讯云 `SECRETKEY` 保存到 Electron 客户端。

## 用户交互

### 设置入口

设置中心新增 `远程 IM` 配置区。

该区域负责配置和状态展示：

- 开启或关闭远程 IM。
- 配置腾讯 IM `SDKAppID`。
- 配置桌面端登录使用的 `UserID`。
- 配置 `UserSig` 签发服务地址。
- 配置允许控制本机的手机端 `UserID` 白名单。
- 展示连接状态、登录状态、最近错误。

设置界面设计图：

- `docs/design/tencent-im-remote-control-ui.png`
- `docs/design/tencent-im-remote-control-ui.svg`

### 会话入口

顶部工具栏新增或复用 `远程 IM` 按钮，点击后打开右侧会话抽屉。

会话抽屉负责运行态交互：

- 显示手机消息、本软件系统消息、AICLI 输出消息。
- 显示每条消息状态：已接收、已发送到 AICLI、回发中、已回发、失败。
- 提供桌面端手动发送消息入口。
- 保持界面简洁，不展示链路图、目标卡片、策略卡片等辅助信息。
- 不提供额外的二次开关。远程 IM 开启并登录成功后，白名单手机消息自动转发给当前 AICLI。

会话界面设计图：

- `docs/design/tencent-im-conversation-drawer-ui.png`
- `docs/design/tencent-im-conversation-drawer-ui.svg`

## 完整交互链路

```text
手机终端
  -> 腾讯云 IM
  -> Multi-AI Code TencentImChannel
  -> RemoteCommandRouter
  -> 当前 AICLI session
  -> AICLI 输出流
  -> OutputBridge
  -> 腾讯云 IM
  -> 手机终端
```

具体步骤：

1. 手机端发送文本消息。
2. 腾讯云 IM 投递消息到桌面端 SDK。
3. `TencentImChannel` 收到消息。
4. `RemoteCommandRouter` 校验发送者是否在白名单内。
5. `RemoteCommandRouter` 查找当前可接收远程消息的主 AICLI session。
6. Multi-AI Code 将手机消息包装后发送给 AICLI。
7. AICLI 在终端中处理任务并持续输出。
8. `OutputBridge` 监听 AICLI 输出，清理控制字符并按时间或长度合并。
9. `TencentImChannel` 将合并后的输出回发给手机端。
10. 本地数据库记录消息、状态和错误。

## 模块设计

新增主进程目录：

```text
electron/remote-im/
  config.ts
  types.ts
  tencentImChannel.ts
  remoteCommandRouter.ts
  outputBridge.ts
  messageStore.ts
  ipc.ts
```

### `config.ts`

负责远程 IM 配置的读取、保存和校验。

配置建议项目级保存，同时保留全局默认值。第一版优先使用当前项目配置，未选择项目时禁用远程控制。

核心字段：

```ts
interface RemoteImConfig {
  enabled: boolean
  provider: 'tencent-im'
  sdkAppId: number | null
  desktopUserId: string
  userSigEndpoint: string
  allowedUserIds: string[]
  outputFlushIntervalMs: number
  outputMaxChunkChars: number
}
```

`enabled` 即远程 IM 总开关。开启后应用自动连接腾讯 IM，并把白名单手机消息转发到当前 AICLI；关闭后断开 IM 并停止转发。

### `tencentImChannel.ts`

负责腾讯 IM SDK 连接。

职责：

- 获取 UserSig。
- 初始化腾讯 IM SDK。
- 登录桌面端 `UserID`。
- 监听连接状态。
- 监听新消息。
- 发送文本消息。
- 断开连接。

腾讯 IM Web SDK V4 使用 `@tencentcloud/lite-chat`。正式环境下 `UserSig` 必须由服务端签发，客户端只调用 `userSigEndpoint` 获取。

官方参考：

- Web SDK：https://cloud.tencent.com/document/product/269/75285
- UserSig：https://cloud.tencent.com/document/product/269/32688

### `remoteCommandRouter.ts`

负责把 IM 消息路由到 AICLI。

职责：

- 丢弃非白名单用户消息。
- 丢弃空消息、超长消息和非文本消息。
- 查找当前主 AICLI session。
- 没有可用 AICLI 时，回发明确错误。
- 复用现有 `sendUserMessageToSession(sessionId, text)`，不直接写 PTY。
- 记录消息投递状态。

发送给 AICLI 的包装格式：

```text
[来自远程 IM：phone_admin]
帮我检查当前项目为什么构建失败，并给出修复方案。
```

第一版不自动创建 AICLI，避免手机端误触发本地任务。

### `outputBridge.ts`

负责把 AICLI 输出回发到手机。

AICLI 输出是 PTY 字符流，不是完整消息。需要做：

- 按 session 订阅 `cc:data` 输出。
- 过滤无关 session。
- 清理 ANSI 控制符和终端控制序列。
- 合并短时间内的输出。
- 按最大长度拆分长消息。
- 避免高频刷屏。
- 在远程 IM 关闭或断开后取消订阅。

默认策略：

- 每 2 秒合并一次输出。
- 单条 IM 消息不超过配置的最大字符数。
- 如果输出过长，分段回发。
- 如果发送失败，记录错误并在会话抽屉显示。

### `messageStore.ts`

负责保存远程 IM 会话记录。

建议新增 SQLite 表：

```sql
CREATE TABLE IF NOT EXISTS remote_im_messages (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  project_id TEXT,
  session_id TEXT,
  provider TEXT NOT NULL,
  remote_message_id TEXT,
  from_user_id TEXT,
  to_user_id TEXT,
  role TEXT NOT NULL,
  direction TEXT NOT NULL,
  content TEXT NOT NULL,
  status TEXT NOT NULL,
  error TEXT,
  created_at INTEGER NOT NULL,
  sent_to_aicli_at INTEGER,
  sent_to_im_at INTEGER
);
```

`role` 可选值：

- `remote-user`
- `system`
- `aicli`

`direction` 可选值：

- `incoming`
- `outgoing`
- `internal`

`status` 可选值：

- `received`
- `rejected`
- `sent-to-aicli`
- `streaming`
- `sent-to-im`
- `failed`

### `ipc.ts`

负责暴露给渲染进程的 IPC。

建议接口：

```ts
remoteIm.getConfig(projectId)
remoteIm.setConfig(projectId, config)
remoteIm.connect(projectId)
remoteIm.disconnect(projectId)
remoteIm.getStatus(projectId)
remoteIm.listMessages(projectId, limit)
remoteIm.sendLocalMessage(projectId, text)
remoteIm.clearSession(projectId)
remoteIm.onStatusChanged(callback)
remoteIm.onMessageChanged(callback)
```

## 渲染层设计

新增组件建议：

```text
src/remote-im/
  RemoteImSettingsSection.tsx
  RemoteImDrawer.tsx
  remoteImViewModel.ts
```

### `RemoteImSettingsSection`

嵌入设置中心，展示配置表单。

重点：

- 不展示聊天记录。
- 不展示腾讯云 `SECRETKEY` 输入项。
- 明确提示 UserSig 需要服务端签发。
- 连接测试按钮只测试配置可用性，不改变远程 IM 开关状态。
- 用户开启远程 IM 后，系统自动进入可远程控制状态，不再要求额外打开第二个状态。

### `RemoteImDrawer`

作为右侧抽屉展示会话记录。

重点：

- 运行态入口，不是配置页。
- 对话记录按角色区分视觉样式。
- 每条消息显示状态。
- 当前目标 AICLI 不存在时，抽屉顶部显示阻断原因。
- 支持手动关闭远程 IM。

### `remoteImViewModel`

负责把主进程状态转换为 UI 状态。

职责：

- 状态标签映射。
- 消息分组。
- 错误提示文案。
- 空状态文案。
- 按钮可用性。

## 安全设计

第一版必须满足：

- `SECRETKEY` 不进入客户端。
- 必须配置白名单 `allowedUserIds`。
- 默认关闭远程 IM。
- 没有独立的二次开关；远程 IM 开启并连接后自动接收白名单用户消息。
- 没有当前 AICLI 时，不自动创建新会话。
- 远程消息只发送给 AICLI，不直接执行本地命令。
- 非白名单消息记录为 `rejected`，不显示为普通待处理消息。
- 远程 IM 状态在 UI 中明确可见。

## 错误处理

需要覆盖这些场景：

- UserSig 服务不可达。
- UserSig 过期或无效。
- 腾讯 IM 登录失败。
- 网络断开。
- 被踢下线。
- 收到非白名单用户消息。
- 当前没有可用 AICLI。
- AICLI session 已退出。
- IM 回发失败。
- 输出过长被拆分。

所有错误应同时进入：

- 会话抽屉状态。
- 本地消息记录。
- 主进程日志。

## 测试计划

### 单元测试

- 配置校验。
- 白名单校验。
- 消息包装格式。
- 无 AICLI 时的错误返回。
- 输出合并和拆分。
- ANSI 控制符清理。
- view model 状态转换。

### IPC 测试

- 读取和保存配置。
- 连接和断开状态。
- 消息列表读取。
- 本地手动发送消息。

### 集成测试

第一阶段可以用可替换的 `RemoteMessageChannel` fake 实现验证：

- 收到手机消息后调用 `sendUserMessageToSession`。
- AICLI 输出后触发回发。
- 白名单之外消息不会进入 AICLI。

真实腾讯 IM 联调作为手动验证：

- 手机端发送消息。
- 桌面端收到并展示。
- 当前 AICLI 收到消息。
- AICLI 输出回发手机。

## 迭代顺序

1. 抽象远程消息通道和类型。
2. 增加配置存储和 IPC。
3. 增加设置中心远程 IM 配置区。
4. 增加右侧远程 IM 会话抽屉。
5. 实现本地 fake 通道测试链路。
6. 接入腾讯 IM SDK。
7. 接入 UserSig 签发服务。
8. 接入 AICLI 输入和输出桥。
9. 完成真实手机端联调。

## V1 默认决策

- UserSig 签发服务第一版使用用户提供的服务地址，本项目不内置公网签发服务，避免误保存腾讯云 `SECRETKEY`。
- 手机端第一版优先使用腾讯 IM 官方 Demo 或用户已有手机端验证，不同步开发手机 Web 页面。
- 远程输出第一版只回发从远程消息被发送给 AICLI 之后产生的输出，不回放历史终端内容。
