# Remote IM Chat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn Remote IM from a single test drawer into a normal IM chat surface with UserID-only contacts and friend/master/slave relationships.

**Architecture:** Keep credentials and local identity in settings. Add a first-version contact model based on UserID relationship lists, derive conversations from configured contacts plus message history, and make the drawer a two-column IM UI. Preserve existing master/slave task routing while allowing friend/master normal peer chat and requiring selected peer sends from the UI.

**Tech Stack:** Electron IPC, TypeScript, React server-rendered component tests, Vitest, Tencent IM runtime already integrated.

---

## File Structure

- Modify `electron/remote-im/types.ts` and `electron/preload.ts`: add friend contact support to remote IM config and API send target.
- Modify `electron/remote-im/config.ts` and `electron/remote-im/config.test.ts`: normalize `friendUserIds`, derive `allowedUserIds`, and validate contacts.
- Modify `electron/remote-im/rolePermissions.ts`, `electron/remote-im/rolePermissions.test.ts`, `electron/remote-im/router.ts`, `electron/remote-im/router.test.ts`, `electron/remote-im/peerMessage.ts`, and `electron/remote-im/peerMessage.test.ts`: support `friend`, preserve master/slave routing, and allow selected-peer sends.
- Modify `src/remote-im/remoteImViewModel.ts` and `src/remote-im/remoteImViewModel.test.ts`: derive contacts, conversations, selected-peer message filtering, and add-contact config updates.
- Modify `src/remote-im/RemoteImDrawer.tsx` and `src/remote-im/RemoteImDrawer.test.tsx`: replace single timeline drawer with two-column IM UI and add-contact dialog.
- Modify `src/App.tsx`: maintain selected Remote IM peer, pass selected target to `sendPeerMessage`, and persist add-contact changes.
- Modify `src/styles.css`: add compact IM layout styles.

---

### Task 1: Config Contact Lists

**Files:**
- Modify: `electron/remote-im/types.ts`
- Modify: `electron/preload.ts`
- Modify: `electron/remote-im/config.ts`
- Test: `electron/remote-im/config.test.ts`

- [ ] **Step 1: Write failing config tests**

Add tests proving `friendUserIds` normalizes, legacy configs still migrate, and `allowedUserIds` includes friend/master/slave UserIDs.

- [ ] **Step 2: Run test to verify failure**

Run: `npx vitest run electron/remote-im/config.test.ts`

Expected: FAIL because `friendUserIds` is not part of the config yet.

- [ ] **Step 3: Add config support**

Add `friendUserIds: string[]` to `RemoteImConfig` in both Electron and preload types. In `normalizeRemoteImConfig`, normalize `friendUserIds` and set `allowedUserIds` to the unique union of friend, master, and slave lists. Keep legacy `allowedUserIds` migrating to `masterUserIds` when explicit role lists are absent.

- [ ] **Step 4: Run test to verify pass**

Run: `npx vitest run electron/remote-im/config.test.ts`

Expected: PASS.

---

### Task 2: Relationship Permissions and Routing

**Files:**
- Modify: `electron/remote-im/rolePermissions.ts`
- Modify: `electron/remote-im/peerMessage.ts`
- Modify: `electron/remote-im/router.ts`
- Test: `electron/remote-im/rolePermissions.test.ts`
- Test: `electron/remote-im/peerMessage.test.ts`
- Test: `electron/remote-im/router.test.ts`

- [ ] **Step 1: Write failing permission tests**

Add tests proving:

- Friend relation resolves as `friend`.
- Manual send from a master is allowed to friend/master/slave contacts.
- Manual send from a slave remains blocked.
- Friend incoming messages are stored as normal IM messages and do not route into AICLI.

- [ ] **Step 2: Run tests to verify failure**

Run: `npx vitest run electron/remote-im/rolePermissions.test.ts electron/remote-im/peerMessage.test.ts electron/remote-im/router.test.ts`

Expected: FAIL because friend relation does not exist and router rejects friends as unknown senders.

- [ ] **Step 3: Implement relation helpers**

Add `RemoteImContactRelation = 'friend' | 'master' | 'slave'`. Resolve relation from `friendUserIds`, `masterUserIds`, and `slaveUserIds`. Keep existing `getRemoteImPeerRole` for task-only master/slave checks.

- [ ] **Step 4: Update routing**

In the router, accept configured friends as normal IM messages. Only master relation can route a task into AICLI. Slave relation can still return system/AICLI output but cannot initiate ordinary tasks.

- [ ] **Step 5: Run tests to verify pass**

Run: `npx vitest run electron/remote-im/rolePermissions.test.ts electron/remote-im/peerMessage.test.ts electron/remote-im/router.test.ts`

Expected: PASS.

---

### Task 3: Conversation View Model

**Files:**
- Modify: `src/remote-im/remoteImViewModel.ts`
- Test: `src/remote-im/remoteImViewModel.test.ts`

- [ ] **Step 1: Write failing view-model tests**

Add tests proving:

- Contacts are derived from `friendUserIds`, `masterUserIds`, and `slaveUserIds`.
- Conversations include peers that only appear in message history.
- Selected conversation messages are filtered by peer UserID.
- Adding a contact by UserID and relation updates the right list and keeps `allowedUserIds` in sync.

- [ ] **Step 2: Run test to verify failure**

Run: `npx vitest run src/remote-im/remoteImViewModel.test.ts`

Expected: FAIL because these helpers do not exist.

- [ ] **Step 3: Implement helpers**

Implement pure helpers:

- `getRemoteImContacts(config)`
- `getRemoteImMessagePeerUserId(message, localUserId)`
- `getRemoteImConversations(config, messages)`
- `filterRemoteImMessagesByPeer(messages, localUserId, peerUserId)`
- `addRemoteImContact(config, relation, userId)`

- [ ] **Step 4: Run test to verify pass**

Run: `npx vitest run src/remote-im/remoteImViewModel.test.ts`

Expected: PASS.

---

### Task 4: Two-Column IM Drawer

**Files:**
- Modify: `src/remote-im/RemoteImDrawer.tsx`
- Modify: `src/styles.css`
- Test: `src/remote-im/RemoteImDrawer.test.tsx`

- [ ] **Step 1: Write failing drawer tests**

Add tests proving the drawer renders:

- A left `会话` column.
- Relationship tabs: `最近`, `好友`, `主人`, `奴隶`.
- UserID rows from config.
- A selected chat header using the selected UserID.
- An add-contact dialog with only relation type and UserID.
- No persistent `联系人资料`.
- No SDKAppID or SecretKey in the add-contact flow.

- [ ] **Step 2: Run test to verify failure**

Run: `npx vitest run src/remote-im/RemoteImDrawer.test.tsx`

Expected: FAIL because current drawer is a single timeline.

- [ ] **Step 3: Implement drawer props and UI**

Update `RemoteImDrawerProps` with selected peer, peer selection handler, selected-peer send handler, and add-contact handler. Render the two-column chat UI and keep Markdown rendering for message content.

- [ ] **Step 4: Add CSS**

Add styles for `remote-im-shell`, `remote-im-sidebar`, `remote-im-chat`, conversation rows, relation chips, and add-contact modal.

- [ ] **Step 5: Run test to verify pass**

Run: `npx vitest run src/remote-im/RemoteImDrawer.test.tsx`

Expected: PASS.

---

### Task 5: App Wiring and Verification

**Files:**
- Modify: `src/App.tsx`
- Modify: `electron/preload.ts`
- Test: `electron/preload.remoteIm.test.ts`
- Test: `src/remote-im/outgoingDelivery.test.ts` only if send event shape changes

- [ ] **Step 1: Write failing integration-facing tests**

Update preload tests so `remoteIm.sendPeerMessage(projectId, text, toUserId)` passes the selected UserID to IPC.

- [ ] **Step 2: Run test to verify failure**

Run: `npx vitest run electron/preload.remoteIm.test.ts`

Expected: FAIL because the third argument is not forwarded yet.

- [ ] **Step 3: Update API and App**

Update `sendPeerMessage` in preload to accept optional `toUserId`. In `App.tsx`, track `remoteImSelectedPeerUserId`, call `sendPeerMessage(currentProjectId, text, remoteImSelectedPeerUserId)`, and persist contacts added from the drawer through `remoteIm.setConfig`.

- [ ] **Step 4: Run focused tests**

Run:

```bash
npx vitest run electron/preload.remoteIm.test.ts src/remote-im electron/remote-im
```

Expected: PASS.

- [ ] **Step 5: Run full verification**

Run:

```bash
npm run typecheck
npm run build
```

Expected: both exit 0.

