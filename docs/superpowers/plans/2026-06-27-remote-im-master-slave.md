# Remote IM Master/Slave Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a master/slave permission model to PC Remote IM so slaves only accept master tasks and automatically return AICLI results.

**Architecture:** Add role fields to `RemoteImConfig`, centralize permission decisions in a small backend helper, and let IPC/router/UI consume that helper instead of open-coded role checks. Existing `allowedUserIds` stays as a legacy compatibility field and migrates to master peers when new role lists are absent.

**Tech Stack:** Electron main process TypeScript, React renderer TypeScript, Vitest.

---

### Task 1: Role-Aware Config

**Files:**
- Modify: `electron/remote-im/types.ts`
- Modify: `electron/remote-im/config.ts`
- Modify: `electron/remote-im/config.test.ts`
- Modify: `electron/preload.ts`
- Modify: `src/App.tsx`

- [ ] Add `RemoteImDesktopRole = 'master' | 'slave'`.
- [ ] Add `desktopRole`, `masterUserIds`, and `slaveUserIds` to config interfaces and defaults.
- [ ] Normalize new lists with trimming and de-dupe.
- [ ] Migrate legacy `allowedUserIds` into `masterUserIds` only when new lists are missing.
- [ ] Validate slave configs require at least one master.
- [ ] Run `npx vitest run electron/remote-im/config.test.ts`.

### Task 2: Central Permission Helper

**Files:**
- Create: `electron/remote-im/rolePermissions.ts`
- Create: `electron/remote-im/rolePermissions.test.ts`
- Modify: `electron/remote-im/peerMessage.ts`
- Modify: `electron/remote-im/peerMessage.test.ts`

- [ ] Test master default peer resolves master first, then slave.
- [ ] Test slave has no manual outbound peer.
- [ ] Test master accepts master and slave inbound tasks.
- [ ] Test slave accepts only master inbound tasks.
- [ ] Implement helper functions for peer role lookup, inbound task authorization, manual send authorization, and default peer resolution.
- [ ] Run `npx vitest run electron/remote-im/rolePermissions.test.ts electron/remote-im/peerMessage.test.ts`.

### Task 3: Backend Routing and IPC Enforcement

**Files:**
- Modify: `electron/remote-im/router.ts`
- Modify: `electron/remote-im/router.test.ts`
- Modify: `electron/remote-im/ipc.ts`

- [ ] Write router tests for slave accepting master task, slave rejecting slave, master accepting master, and unknown sender rejection.
- [ ] Apply role permission checks before routing normal text into AICLI.
- [ ] Keep system notices and remote AICLI output as record-only messages.
- [ ] Block `remote-im:send-peer-message` when local role is slave or destination is not allowed.
- [ ] Run `npx vitest run electron/remote-im/router.test.ts electron/remote-im/peerMessage.test.ts`.

### Task 4: Renderer Settings and Drawer

**Files:**
- Modify: `src/remote-im/RemoteImSettingsSection.tsx`
- Modify: `src/remote-im/RemoteImSettingsSection.test.tsx`
- Modify: `src/remote-im/RemoteImDrawer.tsx`
- Modify: `src/remote-im/RemoteImDrawer.test.tsx`
- Modify: `src/remote-im/remoteImViewModel.ts`
- Modify: `src/remote-im/remoteImViewModel.test.ts`
- Modify: `src/App.tsx`

- [ ] Add settings controls for local role, master UserIDs, and slave UserIDs.
- [ ] Pass `remoteImConfig` into `RemoteImDrawer`.
- [ ] Disable/hide slave manual sending in the drawer.
- [ ] Extend `isRemoteImSendDisabled` with local role.
- [ ] Run `npx vitest run src/remote-im/RemoteImSettingsSection.test.tsx src/remote-im/RemoteImDrawer.test.tsx src/remote-im/remoteImViewModel.test.ts`.

### Task 5: Full Verification and Restart

**Files:**
- No source edits unless verification finds a defect.

- [ ] Run targeted Remote IM tests.
- [ ] Run `npm run typecheck`.
- [ ] Restart the two local Electron clients on ports 9222 and 9223 for manual testing.
