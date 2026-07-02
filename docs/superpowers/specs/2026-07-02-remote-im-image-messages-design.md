# Remote IM 图片消息设计

## 背景

当前 Remote IM 已支持文本消息和语音消息。语音消息会先下载到本地，再转写成文本送给当前 AICLI。

IM SDK 本身提供图片消息能力：

- iOS SDK 有 `createImageMessage(imagePath:)` 和图片下载接口。
- 桌面端 Web SDK 有 `createImageMessage(...)`，但发送图片前需要注册 SDK 上传插件。

现有应用没有把这条链路接起来，所以用户在 iOS 或桌面端都不能像普通 IM 一样发送图片，也不能把收到的图片作为本地文件路径交给 AICLI。

## 目标

- iOS 和桌面端都支持发送图片。
- iOS 和桌面端都支持接收、下载、展示图片。
- 桌面端收到图片后，将图片缓存为本地文件，并把本地图片路径转给当前 AICLI。
- 图片消息在聊天历史里保留可读的占位文案和结构化附件信息。
- UI 不出现已废弃的旧权限模式文案。

## 非目标

- 不做群聊图片消息。
- 不做图片编辑、压缩裁剪、相册管理或多图批量发送。
- 不在应用内做 OCR 或多模态识别。
- 不重新设计联系人权限模型；沿用当前可信联系人路由判断。
- 不把图片二进制写入消息数据库。

## 推荐方案

采用“结构化图片附件 + 本地缓存路径”的方案。

消息本体继续保留 `content` 字段作为列表预览和降级展示，例如 `[图片消息] screenshot.png`。同时新增图片附件数据，用于 UI 展示、下载缓存、重试和 AICLI 路由。

附件结构建议包含：

```ts
export interface RemoteImImageAttachment {
  localPath: string | null
  remoteUrl: string | null
  thumbnailUrl: string | null
  width: number | null
  height: number | null
  sizeBytes: number | null
  fileName: string | null
  mimeType: string | null
  sdkImageId: string | null
}
```

消息增加一个轻量类型字段：

```ts
export type RemoteImMessageKind = 'text' | 'image'
```

数据库可以用两个新增列承载：

- `kind TEXT NOT NULL DEFAULT 'text'`
- `attachment_json TEXT`

这样旧消息天然是文本消息，图片消息也不需要新建独立媒体表。

## 方案取舍

### 方案 A：只把图片 URL 拼进 Markdown

优点是改动最少。

问题是无法稳定保存本地路径、下载状态、缩略图和文件元数据，也不利于把图片路径交给 AICLI。

不采用。

### 方案 B：结构化附件字段

优点是数据边界清晰，旧消息迁移成本低，UI 和路由都能拿到同一份附件信息。

这是推荐方案。

### 方案 C：新增媒体资源表

优点是更适合多附件、多文件和复用资源。

当前只做单图消息，单独建表会增加迁移和查询复杂度。

暂不采用。

## 桌面端数据流

### 发送图片

1. 用户在 Remote IM 聊天输入区点击图片按钮。
2. Renderer 通过文件选择器拿到 `File` 对象，并用 `webUtils.getPathForFile(file)` 获取本地路径。
3. Renderer 把 `File` 放进一个仅在当前窗口内存中的待发送文件注册表，生成 `fileToken`。
4. Renderer 通过 IPC 通知 main 创建一条 outgoing 图片消息，消息中保存本地路径、文件名、大小和 `fileToken`。
5. main 广播 outgoing 图片发送事件。
6. `RemoteImClientHost` 根据 `fileToken` 取回原始 `File` 对象，调用 IM SDK 的 `createImageMessage(...)` 和 `sendMessage(...)`。
7. 发送成功后更新消息状态；失败则保留失败状态和错误信息。

使用 `fileToken` 是因为 `File` 对象不能可靠地跨 IPC 传给 main，而 Web SDK 发送图片需要浏览器侧的 `File`。

### 接收图片

1. `RemoteImClientHost` 从 SDK 收到图片消息。
2. 解析图片元素，优先取原图 URL，同时保留缩略图 URL。
3. 通过 IPC 把远端图片元数据传给 main。
4. main 下载图片到应用缓存目录，例如 `remote-im/images/<projectId>/<messageId>.<ext>`。
5. main 写入图片消息，附件中保存 `localPath` 和远端 URL。
6. 如果发送人是当前项目允许路由到 AICLI 的可信联系人，main 给当前 AICLI 输入一段文本提示：

```text
[图片消息]
来自: <userId>
本地路径: <absolute image path>
请根据图片内容和上下文继续处理。
```

如果图片下载失败，消息仍会入库，但标记错误；不把不可访问的远端 URL 当成本地路径交给 AICLI。

## iOS 数据流

### 发送图片

1. `ChatView` 在输入区增加图片按钮。
2. 使用系统照片选择器选择单张图片。
3. App 将图片复制到应用缓存目录，得到稳定的本地文件路径。
4. `RemoteIMClient` 增加 `sendImage(to:fileURL:)`。
5. SDK 实现调用 `createImageMessage(imagePath:)` 创建图片消息并发送。
6. 本地聊天历史插入 outgoing 图片消息，失败时显示发送失败。

### 接收图片

1. SDK 收到图片消息后解析图片元素。
2. 选择原图或大图资源下载到应用缓存目录。
3. `RemoteIMAppState` 接收图片消息，并写入本地聊天历史。
4. `ChatView` 用图片气泡展示本地图片。

iOS 端只负责聊天体验和转发图片消息，不直接运行 AICLI。图片真正转给 AICLI 的链路发生在桌面端接收后。

## UI 设计

### 桌面端

- 聊天输入区增加一个图片图标按钮。
- 选择图片后直接发送，发送中展示本地缩略图。
- 图片消息用气泡展示图片预览。
- 点击图片可以打开本地缓存文件或系统预览。
- 发送失败时显示现有失败态和重试入口。

### iOS

- 输入区增加图片按钮。
- 图片消息显示为紧凑气泡，保留现有左右对齐规则。
- 发送中和失败态沿用当前消息状态样式。

## 文件限制

第一版建议限制：

- 支持 `jpg`、`jpeg`、`png`、`gif`、`webp`。
- 单张图片最大 20 MB。iOS SDK 上限更高，但桌面和移动端统一限制可以减少上传失败。
- 非图片文件直接拒绝，并显示明确错误。

## 错误处理

- SDK 未注册上传插件：桌面端图片发送入口禁用或发送时提示配置缺失。
- 本地文件不存在：不创建发送任务。
- 上传失败：消息标记为 failed，保留本地预览。
- 下载失败：消息入库并显示失败态，不转给 AICLI。
- 非可信联系人发来的图片：只作为普通聊天消息展示，不路由到 AICLI。
- 当前没有运行中的 AICLI 会话：图片消息入库，但不写入终端输入。

## 测试策略

桌面端按 TDD 增加覆盖：

- SDK 消息解析：图片元素能转换为 incoming 图片事件。
- 消息存储：旧文本消息默认 `kind = text`，图片附件能入库和读取。
- 路由：可信联系人图片下载成功后会把本地路径写给 AICLI；下载失败不会写入。
- 发送：图片消息创建、outgoing 事件、发送成功和失败状态更新。
- UI：图片按钮、图片气泡、发送失败态和文本消息回归。

iOS 端增加核心逻辑测试和构建验证：

- 图片消息模型可保存和读取。
- `RemoteIMAppState` 能处理发送和接收图片消息。
- iOS 工程能通过 `xcodebuild` 构建。

## 验收标准

- 桌面端可以给 iOS 联系人发送图片，iOS 可以正常展示。
- iOS 可以给桌面端发送图片，桌面端可以正常展示。
- 桌面端收到可信联系人图片后，会把本地图片路径发送给当前 AICLI。
- 图片消息重启后仍能在聊天历史中展示。
- README 和各端 UI 不再出现已废弃的旧权限模式文案。
- 现有文本消息和语音消息能力不回退。
