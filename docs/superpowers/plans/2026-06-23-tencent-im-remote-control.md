# Tencent IM Remote Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a V1 Tencent IM remote-control path where phone messages reach the current AICLI session and AICLI output is returned to the phone.

**Architecture:** Keep AICLI routing and message persistence in Electron main, expose typed IPC through preload, and keep the Tencent Web SDK connection in the renderer because it is browser-oriented. The right drawer is intentionally minimal: connection state, message list, text input, and send button.

**Tech Stack:** Electron main/preload, React, TypeScript, better-sqlite3, Vitest, optional `@tencentcloud/lite-chat` runtime import.

---

### Task 1: Remote IM Core Types And Config

**Files:**
- Create: `electron/remote-im/types.ts`
- Create: `electron/remote-im/config.ts`
- Test: `electron/remote-im/config.test.ts`

- [ ] **Step 1: Write failing config tests**

```ts
import { describe, expect, it } from 'vitest'
import {
  DEFAULT_REMOTE_IM_CONFIG,
  normalizeRemoteImConfig,
  validateRemoteImConfig
} from './config.js'

describe('remote IM config', () => {
  it('normalizes missing values to a disabled Tencent IM config', () => {
    expect(normalizeRemoteImConfig(undefined)).toEqual(DEFAULT_REMOTE_IM_CONFIG)
  })

  it('trims user ids and removes empty whitelist entries', () => {
    expect(
      normalizeRemoteImConfig({
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: '1400000000',
        desktopUserId: ' desktop_bot ',
        userSigEndpoint: ' https://example.test/sig ',
        allowedUserIds: [' phone_admin ', '', 'phone_admin'],
        outputFlushIntervalMs: 500,
        outputMaxChunkChars: 10
      })
    ).toMatchObject({
      enabled: true,
      provider: 'tencent-im',
      sdkAppId: 1400000000,
      desktopUserId: 'desktop_bot',
      userSigEndpoint: 'https://example.test/sig',
      allowedUserIds: ['phone_admin'],
      outputFlushIntervalMs: 1000,
      outputMaxChunkChars: 200
    })
  })

  it('rejects enabled configs without required Tencent IM fields', () => {
    const result = validateRemoteImConfig({
      ...DEFAULT_REMOTE_IM_CONFIG,
      enabled: true
    })
    expect(result.ok).toBe(false)
    expect(result.issues.map((issue) => issue.path)).toEqual([
      'sdkAppId',
      'desktopUserId',
      'userSigEndpoint',
      'allowedUserIds'
    ])
  })
})
```

- [ ] **Step 2: Run the failing tests**

Run: `npx vitest run electron/remote-im/config.test.ts`

Expected: FAIL because `electron/remote-im/config.ts` does not exist.

- [ ] **Step 3: Implement types and config normalization**

Create `electron/remote-im/types.ts` with exported config/status/message types, then implement `electron/remote-im/config.ts` with `DEFAULT_REMOTE_IM_CONFIG`, `normalizeRemoteImConfig`, and `validateRemoteImConfig`.

- [ ] **Step 4: Run config tests**

Run: `npx vitest run electron/remote-im/config.test.ts`

Expected: PASS.

### Task 2: Remote IM Message Store

**Files:**
- Modify: `electron/store/db.ts`
- Create: `electron/remote-im/messageStore.ts`
- Test: `electron/remote-im/messageStore.test.ts`

- [ ] **Step 1: Write failing store tests**

Test message insert/list/status update using an in-memory SQLite database or the repo test DB helpers if available.

- [ ] **Step 2: Run the failing store tests**

Run: `npx vitest run electron/remote-im/messageStore.test.ts`

Expected: FAIL because the store does not exist.

- [ ] **Step 3: Implement the `remote_im_messages` schema and store helpers**

Add the table and indexes in `electron/store/db.ts`. Implement insert, list, clear, and status update helpers in `messageStore.ts`.

- [ ] **Step 4: Run store tests**

Run: `npx vitest run electron/remote-im/messageStore.test.ts`

Expected: PASS.

### Task 3: Router And Output Buffer

**Files:**
- Create: `electron/remote-im/router.ts`
- Create: `electron/remote-im/outputBuffer.ts`
- Test: `electron/remote-im/router.test.ts`
- Test: `electron/remote-im/outputBuffer.test.ts`

- [ ] **Step 1: Write failing router and output-buffer tests**

Cover whitelist rejection, missing AICLI response, prompt wrapping, ANSI stripping, timed output flushing, and max-length splitting.

- [ ] **Step 2: Run failing tests**

Run: `npx vitest run electron/remote-im/router.test.ts electron/remote-im/outputBuffer.test.ts`

Expected: FAIL because the modules do not exist.

- [ ] **Step 3: Implement router and output buffer**

The router receives normalized incoming messages and injected dependencies for active session lookup, AICLI send, store, and IM send. The output buffer converts PTY chunks into clean text chunks for IM.

- [ ] **Step 4: Run router tests**

Run: `npx vitest run electron/remote-im/router.test.ts electron/remote-im/outputBuffer.test.ts`

Expected: PASS.

### Task 4: Main IPC And Preload API

**Files:**
- Create: `electron/remote-im/ipc.ts`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`
- Test: `electron/preload.remoteIm.test.ts`

- [ ] **Step 1: Write failing preload API tests**

Assert `window.api.remoteIm` exposes config, status, message, send, connect-event, and SDK-bridge methods.

- [ ] **Step 2: Run failing preload tests**

Run: `npx vitest run electron/preload.remoteIm.test.ts`

Expected: FAIL because `remoteIm` is not exposed.

- [ ] **Step 3: Implement IPC registration and preload API**

Register main-process handlers for config persistence, message listing, local send, renderer incoming message delivery, SDK status updates, and output forwarding.

- [ ] **Step 4: Run preload and remote-im tests**

Run: `npx vitest run electron/preload.remoteIm.test.ts electron/remote-im/*.test.ts`

Expected: PASS.

### Task 5: Renderer View Model And Drawer

**Files:**
- Create: `src/remote-im/remoteImViewModel.ts`
- Create: `src/remote-im/RemoteImDrawer.tsx`
- Create: `src/remote-im/RemoteImClientHost.tsx`
- Test: `src/remote-im/remoteImViewModel.test.ts`
- Test: `src/remote-im/RemoteImDrawer.test.tsx`

- [ ] **Step 1: Write failing view-model and drawer tests**

Cover minimal drawer rendering, message roles/status labels, disabled input when disconnected, and send button state.

- [ ] **Step 2: Run failing renderer tests**

Run: `npx vitest run src/remote-im/remoteImViewModel.test.ts src/remote-im/RemoteImDrawer.test.tsx`

Expected: FAIL because components do not exist.

- [ ] **Step 3: Implement minimal drawer and client host**

Render only the title/status, message list, input, and send button. `RemoteImClientHost` owns the optional Tencent SDK connection lifecycle.

- [ ] **Step 4: Run renderer tests**

Run: `npx vitest run src/remote-im/remoteImViewModel.test.ts src/remote-im/RemoteImDrawer.test.tsx`

Expected: PASS.

### Task 6: Settings Section And App Integration

**Files:**
- Modify: `src/components/AiSettingsDialog.tsx`
- Modify: `src/components/AiSettingsDialog.test.tsx`
- Modify: `src/App.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Write failing integration tests**

Add tests proving settings has a `远程 IM` section, the top `远程 IM` button opens the drawer, and the drawer stays minimal.

- [ ] **Step 2: Run failing integration tests**

Run: `npx vitest run src/components/AiSettingsDialog.test.tsx src/App.habitMonitor.test.tsx src/remote-im/RemoteImDrawer.test.tsx`

Expected: FAIL because UI integration is missing.

- [ ] **Step 3: Wire UI into settings and app shell**

Add remote IM settings to the settings dialog, add the toolbar button, mount `RemoteImDrawer`, and mount `RemoteImClientHost`.

- [ ] **Step 4: Run integration tests**

Run: `npx vitest run src/components/AiSettingsDialog.test.tsx src/App.habitMonitor.test.tsx src/remote-im/RemoteImDrawer.test.tsx`

Expected: PASS.

### Task 7: Final Verification

**Files:**
- Modify as needed based on failures.

- [ ] **Step 1: Run focused tests**

Run: `npx vitest run electron/remote-im/*.test.ts electron/preload.remoteIm.test.ts src/remote-im/*.test.ts src/components/AiSettingsDialog.test.tsx`

Expected: PASS.

- [ ] **Step 2: Run typecheck**

Run: `npm run typecheck`

Expected: PASS.

- [ ] **Step 3: Run build**

Run: `npm run build`

Expected: PASS. Existing Vite dynamic/static import warnings are acceptable if exit code is 0.

