# Remote IM 图片消息实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让桌面端和 iOS Remote IM 都支持图片收发与展示；桌面端收到可信联系人图片后下载到本地，并把本地图片路径转给当前 AICLI。

**Architecture:** 图片消息使用 `kind + attachment` 扩展现有消息模型，旧文本消息默认 `kind = text`。桌面端 main 负责消息入库、下载缓存、AICLI 路由和状态更新；renderer 保留浏览器 `File` 对象并调用 Web SDK 发送。iOS 端用系统照片选择器复制图片到缓存目录，再通过 SDK 图片消息 API 发送；收到图片后下载到缓存并写入本地聊天历史。

**Tech Stack:** Electron IPC, SQLite, TypeScript, React, Vitest, SwiftUI, PhotosUI, iOS SDK.

---

## File Structure

- Modify `electron/remote-im/types.ts`, `electron/preload.ts`: add image message attachment and incoming/outgoing event types.
- Modify `electron/store/db.ts`, `electron/remote-im/messageStore.ts`, `electron/remote-im/messageStore.test.ts`: add `kind` and `attachment_json`.
- Add `electron/remote-im/imageCache.ts` and tests: download/cache remote images safely.
- Modify `electron/remote-im/router.ts`, `electron/remote-im/router.test.ts`, `electron/remote-im/ipc.ts`: route incoming image messages to storage and AICLI path prompt.
- Modify `src/remote-im/tencentImClient.ts` and tests: extract image SDK messages and send browser `File` images.
- Add `src/remote-im/outgoingImageRegistry.ts`.
- Modify `src/remote-im/outgoingDelivery.ts` and tests: deliver outgoing image events.
- Modify `src/remote-im/RemoteImClientHost.tsx`, `src/App.tsx`, `src/remote-im/RemoteImDrawer.tsx`, `src/remote-im/remoteImViewModel.ts`, `src/styles.css`, and focused tests: image picker, image bubble, image preview.
- Modify iOS files `RemoteIMClient.swift`, `TencentIMClient.swift`, `RemoteIMAppState.swift`, `ChatView.swift`, `MasterChatState.swift`, and iOS tests.

---

### Task 1: Desktop Message Model And Store

**Files:**
- Modify: `electron/remote-im/types.ts`
- Modify: `electron/preload.ts`
- Modify: `electron/store/db.ts`
- Modify: `electron/remote-im/messageStore.ts`
- Test: `electron/remote-im/messageStore.test.ts`

- [x] **Step 1: Write failing store tests**

Add tests proving:

- Legacy text messages map to `kind: 'text'` and `attachment: null`.
- Image messages persist and read image attachment JSON.
- Malformed attachment JSON falls back to `attachment: null` instead of crashing.

- [x] **Step 2: Run store tests to verify failure**

Run: `npx vitest run electron/remote-im/messageStore.test.ts`

Expected: FAIL because `kind` and attachment fields do not exist.

- [x] **Step 3: Add model and DB migration**

Add `RemoteImMessageKind`, `RemoteImImageAttachment`, `RemoteImMessageAttachment`, and optional `attachment` on `RemoteImMessage`. Add `kind` and `attachment_json` columns to schema and `ensureColumn`.

- [x] **Step 4: Run store tests to verify pass**

Run: `npx vitest run electron/remote-im/messageStore.test.ts`

Expected: PASS.

---

### Task 2: Desktop Incoming Image Routing

**Files:**
- Add: `electron/remote-im/imageCache.ts`
- Test: `electron/remote-im/imageCache.test.ts`
- Modify: `electron/remote-im/router.ts`
- Test: `electron/remote-im/router.test.ts`
- Modify: `electron/remote-im/ipc.ts`
- Modify: `electron/preload.ts`

- [x] **Step 1: Write failing cache and router tests**

Add tests proving:

- A remote image URL is downloaded to a deterministic cache path with safe file names.
- Trusted incoming image stores a `kind: image` message and sends an AICLI prompt containing the local path.
- Download failure stores a failed image message and does not send any AICLI input.
- Unknown sender image is rejected like text/audio.

- [x] **Step 2: Run tests to verify failure**

Run: `npx vitest run electron/remote-im/imageCache.test.ts electron/remote-im/router.test.ts`

Expected: FAIL because the cache helper and router image path do not exist.

- [x] **Step 3: Implement image cache and router**

Add incoming image type, cache helper, `handleIncomingImage`, and IPC handler `remote-im:deliver-incoming-image`.

- [x] **Step 4: Run tests to verify pass**

Run: `npx vitest run electron/remote-im/imageCache.test.ts electron/remote-im/router.test.ts`

Expected: PASS.

---

### Task 3: Desktop SDK Image Extract And Send

**Files:**
- Modify: `src/remote-im/tencentImClient.ts`
- Test: `src/remote-im/tencentImClient.test.ts`
- Add: `src/remote-im/outgoingImageRegistry.ts`
- Modify: `src/remote-im/outgoingDelivery.ts`
- Test: `src/remote-im/outgoingDelivery.test.ts`
- Modify: `src/remote-im/RemoteImClientHost.tsx`
- Modify: `electron/preload.ts`
- Modify: `electron/remote-im/ipc.ts`

- [x] **Step 1: Write failing SDK and delivery tests**

Add tests proving:

- SDK image payloads with image arrays convert to incoming image events.
- Outgoing image delivery fails cleanly when runtime is missing or `fileToken` cannot resolve.
- Outgoing image delivery calls runtime `sendImage`.

- [x] **Step 2: Run tests to verify failure**

Run: `npx vitest run src/remote-im/tencentImClient.test.ts src/remote-im/outgoingDelivery.test.ts`

Expected: FAIL because image extraction and delivery do not exist.

- [x] **Step 3: Implement SDK extraction and delivery**

Add `sendImage` to runtime, implement `createImageMessage(...)`, introduce renderer-only file registry, outgoing image IPC event, and status updates.

- [x] **Step 4: Run tests to verify pass**

Run: `npx vitest run src/remote-im/tencentImClient.test.ts src/remote-im/outgoingDelivery.test.ts`

Expected: PASS.

---

### Task 4: Desktop UI

**Files:**
- Modify: `src/remote-im/RemoteImDrawer.tsx`
- Test: `src/remote-im/RemoteImDrawer.test.tsx`
- Modify: `src/remote-im/remoteImViewModel.ts`
- Test: `src/remote-im/remoteImViewModel.test.ts`
- Modify: `src/App.tsx`
- Modify: `src/styles.css`

- [x] **Step 1: Write failing UI tests**

Add tests proving:

- Composer renders an image button.
- Selecting a valid image calls the image send handler.
- Image messages render an image preview.
- Conversation preview for image messages shows `[图片消息]`.

- [x] **Step 2: Run UI tests to verify failure**

Run: `npx vitest run src/remote-im/RemoteImDrawer.test.tsx src/remote-im/remoteImViewModel.test.ts`

Expected: FAIL because UI does not expose image controls.

- [x] **Step 3: Implement UI and app wiring**

Add hidden file input, image button, file validation, `onSendImage`, image bubbles, and style rules.

- [x] **Step 4: Run UI tests to verify pass**

Run: `npx vitest run src/remote-im/RemoteImDrawer.test.tsx src/remote-im/remoteImViewModel.test.ts`

Expected: PASS.

---

### Task 5: iOS Image Model, SDK, And UI

**Files:**
- Modify: `ios/MultiAIIM/MultiAIIMCore/MasterChatState.swift`
- Test: `ios/MultiAIIM/MultiAIIMTests/MasterChatStateTests.swift`
- Modify: `ios/MultiAIIM/MultiAIIM/RemoteIMClient.swift`
- Modify: `ios/MultiAIIM/MultiAIIM/TencentIMClient.swift`
- Modify: `ios/MultiAIIM/MultiAIIM/RemoteIMAppState.swift`
- Modify: `ios/MultiAIIM/MultiAIIM/ChatView.swift`
- Test: existing iOS test target.

- [x] **Step 1: Write failing core tests**

Add tests proving:

- Outgoing image messages carry local path attachment and pending status.
- Incoming image messages carry local path attachment and received status.
- JSON history round-trips image attachment data.

- [x] **Step 2: Run iOS tests to verify failure**

Run: `cd ios/MultiAIIM && xcodebuild test -workspace MultiAIIM.xcworkspace -scheme MultiAIIMCoreTests -destination 'platform=iOS Simulator,name=iPhone 17'`

Expected: FAIL because image message fields do not exist.

- [x] **Step 3: Implement iOS model, app state, SDK and UI**

Add `RemoteIMImageAttachment`, `sendImage`, SDK image send/receive/download, `PhotosPicker`, and image bubble rendering.

- [x] **Step 4: Run iOS tests to verify pass**

Run the same `xcodebuild test` command.

Expected: PASS.

---

### Task 6: Final Verification

- [x] Run targeted desktop tests:

`npx vitest run electron/remote-im/messageStore.test.ts electron/remote-im/imageCache.test.ts electron/remote-im/router.test.ts src/remote-im/tencentImClient.test.ts src/remote-im/outgoingDelivery.test.ts src/remote-im/remoteImViewModel.test.ts src/remote-im/RemoteImDrawer.test.tsx electron/preload.remoteIm.test.ts electron/remote-im/peerMessage.test.ts electron/remote-im/imcliServer.test.ts`

- [x] Run TypeScript build/check command available in the repo.
- [x] Run iOS build/test command, or report the exact blocker if the local simulator/signing environment prevents it.
- [x] Check no README or UI text reintroduces old IM permission labels or Tencent wording.
- [ ] Commit and push the completed change.
