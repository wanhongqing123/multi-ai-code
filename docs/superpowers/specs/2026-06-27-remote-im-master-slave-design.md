# Remote IM Master/Slave Design

## Goal

PC 端 Remote IM 增加明确的主人/奴隶权限模型，避免多个 Multi-AI Code 客户端之间无限互相转发消息。

## Roles

- `master`: 主人节点。可以给主人或奴隶发送普通 IM 消息。
- `slave`: 奴隶节点。不能手动主动发起普通 IM 消息，只能接收主人任务，并把当前 AICLI 的处理结果自动回传给发起任务的主人。

## Configuration

`RemoteImConfig` 增加：

- `desktopRole: 'master' | 'slave'`
- `masterUserIds: string[]`
- `slaveUserIds: string[]`

保留 `allowedUserIds` 作为兼容旧配置的字段。读取旧配置时，如果没有新字段，则把旧的 `allowedUserIds` 迁移到 `masterUserIds`，保证升级后不会默认把未知对端当成奴隶控制。

## Permission Rules

- master -> master: 允许手动互发，收到后进入 AICLI。
- master -> slave: 允许手动发送任务，奴隶收到后进入 AICLI。
- slave -> master: 禁止手动主动发送；AICLI 输出桥允许把结果回传给发起任务的 master。
- slave -> slave: 禁止通信，收到后拒绝，不进入 AICLI。
- unknown -> any: 拒绝，不进入 AICLI。

远程 AICLI 输出和系统通知继续只记录到消息面板，不再路由回本机 AICLI。

## Backend Changes

- 新增角色权限 helper，集中判断对端角色、入站权限、手动出站权限和默认发送目标。
- `validateRemoteImConfig` 校验启用状态下至少存在可通信对端；奴隶模式必须配置至少一个主人。
- `handleIncomingText` 在白名单之前改成角色判断。被拒绝的消息保存为 `rejected`，并尽量回发系统提示。
- `send-peer-message` 使用出站权限判断，奴隶模式直接返回失败，不创建消息、不广播 IM。
- AICLI output forwarding 仍由成功进入 AICLI 的入站任务启动，目标固定为该任务的 `fromUserId`。

## UI Changes

- 设置页增加“本机角色”选择。
- 设置页增加“主人 UserID”和“奴隶 UserID”两个列表。
- 远程 IM 面板收到 `config`，奴隶模式下禁用手动输入和发送按钮，并显示等待主人任务的占位提示。

## Testing

- Config 测试覆盖旧配置迁移、新字段归一化、奴隶缺少主人时校验失败。
- Permission/helper 测试覆盖四种角色通信关系。
- Router 测试覆盖 slave 接收 master 任务、slave 拒绝 slave、master 接收 master、unknown 拒绝。
- Peer message/IPC 测试覆盖 slave 不能手动发送，master 默认发送目标正确。
- React 静态渲染测试覆盖设置项和奴隶抽屉禁用状态。
