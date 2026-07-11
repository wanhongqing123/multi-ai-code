# IM AICLI Control Command Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a source-level IM control command path for Codex/OpenCode and add mobile slash-command suggestions so users can safely send `/status`, `/plan`, and `/build` from IM.

**Architecture:** Desktop owns command execution because it hosts AICLI sessions. Incoming IM text is first classified as either a control command or a normal task; control commands bypass prompt wrapping and reply-marker forwarding, while normal messages keep the current `<remote-im-reply id="...">` flow. iOS and Android only add input suggestions that send the same text commands through existing IM SDK text messages.

**Tech Stack:** Electron main process TypeScript, Codex Rust TUI source, OpenCode TypeScript source/API, SwiftUI iOS app, Android Java app, Vitest, XCTest/Java unit tests where available.

---

## File Structure

- Create `electron/remote-im/controlCommands.ts`
  - Parse exact IM slash commands.
  - Define the white list and display labels shared by Desktop tests.
- Create `electron/remote-im/controlBridge.ts`
  - Host-side dispatcher from parsed command to Codex/OpenCode-specific control implementation.
  - Return a compact message that Desktop sends back to IM directly.
- Modify `electron/remote-im/ipc.ts`
  - Intercept `remote-im:deliver-incoming-text` before `createRemoteImRouter(...).handleIncomingText(...)`.
  - If command is handled, send a system message back to IM and do not start output forwarding.
- Modify `electron/remote-im/router.ts`
  - Keep normal-message behavior unchanged; add tests only if a helper needs dependency injection.
- Add tests:
  - `electron/remote-im/controlCommands.test.ts`
  - `electron/remote-im/controlBridge.test.ts`
  - Update or add IPC-level test if current test harness has coverage for `deliver-incoming-text`.
- Modify Codex submodule:
  - `third_party/aicli/codex/codex-rs/tui/src/...`
  - Add a source-level control entrypoint for status/plan/build/model/usage/diff. If existing internal handlers are not cleanly callable, create a narrow adapter and leave TUI slash handling intact.
- Modify OpenCode submodule:
  - Use existing `session.switchAgent` for `plan` and `build`.
  - Add or expose a small local control helper if the host cannot call the existing generated client directly.
- Modify iOS:
  - `ios/MultiAIIM/MultiAIIM/ChatView.swift`
  - Optional new helper file `ios/MultiAIIM/MultiAIIM/RemoteIMCommandSuggestion.swift`
- Modify Android:
  - `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/MainActivity.java`
  - Optional new helper file `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMCommandSuggestion.java`

---

## Task 1: Desktop Command Parser

**Files:**
- Create: `electron/remote-im/controlCommands.ts`
- Test: `electron/remote-im/controlCommands.test.ts`

- [ ] **Step 1: Write parser tests**

Create `electron/remote-im/controlCommands.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import {
  REMOTE_IM_CONTROL_COMMANDS,
  parseRemoteImControlCommand,
  shouldShowRemoteImCommandSuggestions
} from './controlCommands.js'

describe('remote IM control commands', () => {
  it('parses exact whitelisted commands', () => {
    expect(parseRemoteImControlCommand('/status')).toEqual({ name: 'status', raw: '/status', args: [] })
    expect(parseRemoteImControlCommand('/plan')).toEqual({ name: 'plan', raw: '/plan', args: [] })
    expect(parseRemoteImControlCommand('/build')).toEqual({ name: 'build', raw: '/build', args: [] })
    expect(parseRemoteImControlCommand('/model')).toEqual({ name: 'model', raw: '/model', args: [] })
    expect(parseRemoteImControlCommand('/usage')).toEqual({ name: 'usage', raw: '/usage', args: [] })
    expect(parseRemoteImControlCommand('/diff')).toEqual({ name: 'diff', raw: '/diff', args: [] })
  })

  it('parses model arguments without allowing arbitrary commands', () => {
    expect(parseRemoteImControlCommand('/model codex/gpt-5.5')).toEqual({
      name: 'model',
      raw: '/model codex/gpt-5.5',
      args: ['codex/gpt-5.5']
    })
    expect(parseRemoteImControlCommand('/agent build')).toBeNull()
    expect(parseRemoteImControlCommand('/stop')).toBeNull()
  })

  it('does not trigger from normal text', () => {
    expect(parseRemoteImControlCommand('请帮我看 /status')).toBeNull()
    expect(parseRemoteImControlCommand('/status  ')).toEqual({ name: 'status', raw: '/status', args: [] })
    expect(parseRemoteImControlCommand('/status now')).toBeNull()
    expect(parseRemoteImControlCommand('')).toBeNull()
  })

  it('exposes mobile suggestions in display order', () => {
    expect(REMOTE_IM_CONTROL_COMMANDS.map((item) => item.command)).toEqual([
      '/status',
      '/plan',
      '/build',
      '/model',
      '/usage',
      '/diff'
    ])
  })

  it('shows suggestions only when the draft is slash-prefixed without spaces', () => {
    expect(shouldShowRemoteImCommandSuggestions('/')).toBe(true)
    expect(shouldShowRemoteImCommandSuggestions('/st')).toBe(true)
    expect(shouldShowRemoteImCommandSuggestions('/status')).toBe(true)
    expect(shouldShowRemoteImCommandSuggestions('/status now')).toBe(false)
    expect(shouldShowRemoteImCommandSuggestions(' /')).toBe(false)
    expect(shouldShowRemoteImCommandSuggestions('hello')).toBe(false)
  })
})
```

- [ ] **Step 2: Run parser tests and confirm failure**

Run:

```bash
npm test -- electron/remote-im/controlCommands.test.ts
```

Expected: fail because `controlCommands.ts` does not exist.

- [ ] **Step 3: Implement parser**

Create `electron/remote-im/controlCommands.ts`:

```ts
export type RemoteImControlCommandName = 'status' | 'plan' | 'build' | 'model' | 'usage' | 'diff'

export interface RemoteImControlCommandDefinition {
  name: RemoteImControlCommandName
  command: string
  title: string
  description: string
}

export interface RemoteImControlCommand {
  name: RemoteImControlCommandName
  raw: string
  args: string[]
}

export const REMOTE_IM_CONTROL_COMMANDS: RemoteImControlCommandDefinition[] = [
  { name: 'status', command: '/status', title: '状态', description: '查看当前 AICLI 状态' },
  { name: 'plan', command: '/plan', title: '计划模式', description: '切换到计划模式' },
  { name: 'build', command: '/build', title: '执行模式', description: '切换到执行模式' },
  { name: 'model', command: '/model', title: '模型', description: '查看当前模型' },
  { name: 'usage', command: '/usage', title: '用量', description: '查看当前用量' },
  { name: 'diff', command: '/diff', title: '改动', description: '查看当前工作区改动' }
]

const COMMAND_BY_TEXT = new Map(REMOTE_IM_CONTROL_COMMANDS.map((item) => [item.command, item]))

export function parseRemoteImControlCommand(text: string): RemoteImControlCommand | null {
  const trimmed = text.trim()
  if (!trimmed.startsWith('/')) return null

  const parts = trimmed.split(/\s+/)
  const commandText = parts[0]
  const definition = COMMAND_BY_TEXT.get(commandText)
  if (!definition) return null

  const args = parts.slice(1)
  if (definition.name !== 'model' && args.length > 0) return null
  if (definition.name === 'model' && args.length > 1) return null

  return {
    name: definition.name,
    raw: args.length ? `${commandText} ${args.join(' ')}` : commandText,
    args
  }
}

export function shouldShowRemoteImCommandSuggestions(draft: string): boolean {
  if (!draft.startsWith('/')) return false
  return !/\s/.test(draft)
}
```

- [ ] **Step 4: Verify parser tests pass**

Run:

```bash
npm test -- electron/remote-im/controlCommands.test.ts
```

Expected: pass.

- [ ] **Step 5: Commit parser**

```bash
git add electron/remote-im/controlCommands.ts electron/remote-im/controlCommands.test.ts
git commit -m "OPTIMIZE: 增加 IM 控制命令解析"
```

---

## Task 2: Desktop Control Bridge

**Files:**
- Create: `electron/remote-im/controlBridge.ts`
- Test: `electron/remote-im/controlBridge.test.ts`

- [ ] **Step 1: Write control bridge tests**

Create `electron/remote-im/controlBridge.test.ts`:

```ts
import { describe, expect, it, vi } from 'vitest'
import { executeRemoteImControlCommand } from './controlBridge.js'

describe('remote IM control bridge', () => {
  it('rejects missing sessions', async () => {
    const result = await executeRemoteImControlCommand({
      command: { name: 'status', raw: '/status', args: [] },
      session: null,
      getSourceKind: () => 'codex',
      codex: {},
      opencode: {}
    })
    expect(result).toEqual({ ok: false, message: '当前没有可控制的 AICLI 会话。' })
  })

  it('dispatches opencode plan and build through source-level agent switching', async () => {
    const switchAgent = vi.fn(async () => ({ ok: true as const }))
    const plan = await executeRemoteImControlCommand({
      command: { name: 'plan', raw: '/plan', args: [] },
      session: { sessionId: 's1', command: 'opencode' },
      getSourceKind: () => 'opencode',
      codex: {},
      opencode: { switchAgent }
    })
    const build = await executeRemoteImControlCommand({
      command: { name: 'build', raw: '/build', args: [] },
      session: { sessionId: 's1', command: 'opencode' },
      getSourceKind: () => 'opencode',
      codex: {},
      opencode: { switchAgent }
    })

    expect(switchAgent).toHaveBeenNthCalledWith(1, 's1', 'plan')
    expect(switchAgent).toHaveBeenNthCalledWith(2, 's1', 'build')
    expect(plan.message).toBe('已切换到 OpenCode plan 模式。')
    expect(build.message).toBe('已切换到 OpenCode build 模式。')
  })

  it('does not expose codex stop and reports codex build until source support exists', async () => {
    const result = await executeRemoteImControlCommand({
      command: { name: 'build', raw: '/build', args: [] },
      session: { sessionId: 's1', command: 'codex' },
      getSourceKind: () => 'codex',
      codex: {},
      opencode: {}
    })
    expect(result).toEqual({ ok: false, message: 'Codex 暂未支持 build 模式切换。' })
  })

  it('formats status from provider data', async () => {
    const result = await executeRemoteImControlCommand({
      command: { name: 'status', raw: '/status', args: [] },
      session: { sessionId: 's1', command: 'opencode' },
      getSourceKind: () => 'opencode',
      codex: {},
      opencode: {
        getStatus: async () => ({
          ok: true as const,
          aicli: 'OpenCode',
          sessionId: 'ses_1',
          mode: 'build',
          model: 'idealab/Qwen3.7-Max-DogFooding',
          state: 'idle'
        })
      }
    })
    expect(result.message).toContain('当前 AICLI：OpenCode')
    expect(result.message).toContain('模式：build')
  })
})
```

- [ ] **Step 2: Run bridge tests and confirm failure**

Run:

```bash
npm test -- electron/remote-im/controlBridge.test.ts
```

Expected: fail because `controlBridge.ts` does not exist.

- [ ] **Step 3: Implement control bridge**

Create `electron/remote-im/controlBridge.ts`:

```ts
import type { RemoteImAicliOutputSourceKind } from './aicliOutputSanitizer.js'
import type { RemoteImControlCommand } from './controlCommands.js'

export interface RemoteImControlSession {
  sessionId: string
  command?: string
}

export interface RemoteImControlStatus {
  ok: true
  aicli: string
  sessionId: string
  mode?: string
  model?: string
  state?: string
}

export interface RemoteImControlBridgeDeps {
  command: RemoteImControlCommand
  session: RemoteImControlSession | null
  getSourceKind(command: string | undefined): RemoteImAicliOutputSourceKind
  codex: {
    getStatus?: (sessionId: string) => Promise<RemoteImControlStatus | { ok: false; error: string }>
    enterPlan?: (sessionId: string) => Promise<{ ok: boolean; error?: string }>
    enterBuild?: (sessionId: string) => Promise<{ ok: boolean; error?: string }>
    getModel?: (sessionId: string) => Promise<{ ok: boolean; message?: string; error?: string }>
    getUsage?: (sessionId: string) => Promise<{ ok: boolean; message?: string; error?: string }>
    getDiff?: (sessionId: string) => Promise<{ ok: boolean; message?: string; error?: string }>
  }
  opencode: {
    getStatus?: (sessionId: string) => Promise<RemoteImControlStatus | { ok: false; error: string }>
    switchAgent?: (sessionId: string, agent: 'plan' | 'build') => Promise<{ ok: boolean; error?: string }>
    getModel?: (sessionId: string) => Promise<{ ok: boolean; message?: string; error?: string }>
    getUsage?: (sessionId: string) => Promise<{ ok: boolean; message?: string; error?: string }>
    getDiff?: (sessionId: string) => Promise<{ ok: boolean; message?: string; error?: string }>
  }
}

export interface RemoteImControlResult {
  ok: boolean
  message: string
}

function formatStatus(status: RemoteImControlStatus): string {
  return [
    `当前 AICLI：${status.aicli}`,
    `Session：${status.sessionId}`,
    status.mode ? `模式：${status.mode}` : null,
    status.model ? `模型：${status.model}` : null,
    status.state ? `状态：${status.state}` : null
  ]
    .filter((line): line is string => Boolean(line))
    .join('\n')
}

function unsupported(aicli: string, command: string): RemoteImControlResult {
  return { ok: false, message: `${aicli} 暂不支持 ${command}。` }
}

export async function executeRemoteImControlCommand(
  deps: RemoteImControlBridgeDeps
): Promise<RemoteImControlResult> {
  const session = deps.session
  if (!session) return { ok: false, message: '当前没有可控制的 AICLI 会话。' }

  const sourceKind = deps.getSourceKind(session.command)
  if (sourceKind === 'claude') {
    return { ok: false, message: 'Claude 暂不支持源码级 IM 控制命令。' }
  }
  if (sourceKind !== 'codex' && sourceKind !== 'opencode') {
    return { ok: false, message: '当前 AICLI 类型未知，无法执行控制命令。' }
  }

  if (sourceKind === 'opencode') {
    if (deps.command.name === 'plan' || deps.command.name === 'build') {
      const switchAgent = deps.opencode.switchAgent
      if (!switchAgent) return unsupported('OpenCode', deps.command.raw)
      const agent = deps.command.name
      const result = await switchAgent(session.sessionId, agent)
      if (!result.ok) return { ok: false, message: result.error ?? `切换 OpenCode ${agent} 模式失败。` }
      return { ok: true, message: `已切换到 OpenCode ${agent} 模式。` }
    }
    if (deps.command.name === 'status') {
      const result = await deps.opencode.getStatus?.(session.sessionId)
      if (!result) return unsupported('OpenCode', deps.command.raw)
      if (!result.ok) return { ok: false, message: result.error }
      return { ok: true, message: formatStatus(result) }
    }
    const helper = deps.opencode[`get${capitalize(deps.command.name)}` as keyof typeof deps.opencode]
    if (typeof helper === 'function') {
      const result = await (helper as (sessionId: string) => Promise<{ ok: boolean; message?: string; error?: string }>)(
        session.sessionId
      )
      return { ok: result.ok, message: result.message ?? result.error ?? '命令执行完成。' }
    }
    return unsupported('OpenCode', deps.command.raw)
  }

  if (deps.command.name === 'build' && !deps.codex.enterBuild) {
    return { ok: false, message: 'Codex 暂未支持 build 模式切换。' }
  }
  if (deps.command.name === 'plan') {
    const result = await deps.codex.enterPlan?.(session.sessionId)
    if (!result) return unsupported('Codex', deps.command.raw)
    return { ok: result.ok, message: result.ok ? '已切换到 Codex plan 模式。' : result.error ?? '切换 Codex plan 模式失败。' }
  }
  if (deps.command.name === 'build') {
    const result = await deps.codex.enterBuild?.(session.sessionId)
    if (!result) return unsupported('Codex', deps.command.raw)
    return { ok: result.ok, message: result.ok ? '已切换到 Codex build 模式。' : result.error ?? '切换 Codex build 模式失败。' }
  }
  if (deps.command.name === 'status') {
    const result = await deps.codex.getStatus?.(session.sessionId)
    if (!result) return unsupported('Codex', deps.command.raw)
    if (!result.ok) return { ok: false, message: result.error }
    return { ok: true, message: formatStatus(result) }
  }

  const helper = deps.codex[`get${capitalize(deps.command.name)}` as keyof typeof deps.codex]
  if (typeof helper === 'function') {
    const result = await (helper as (sessionId: string) => Promise<{ ok: boolean; message?: string; error?: string }>)(
      session.sessionId
    )
    return { ok: result.ok, message: result.message ?? result.error ?? '命令执行完成。' }
  }
  return unsupported('Codex', deps.command.raw)
}

function capitalize(value: string): string {
  return value.charAt(0).toUpperCase() + value.slice(1)
}
```

- [ ] **Step 4: Verify bridge tests pass**

Run:

```bash
npm test -- electron/remote-im/controlBridge.test.ts
```

Expected: pass.

- [ ] **Step 5: Commit bridge**

```bash
git add electron/remote-im/controlBridge.ts electron/remote-im/controlBridge.test.ts
git commit -m "OPTIMIZE: 增加 IM AICLI 控制桥"
```

---

## Task 3: Desktop Incoming IM Control Routing

**Files:**
- Modify: `electron/remote-im/ipc.ts`
- Test: add focused test beside existing remote-im IPC/router tests

- [ ] **Step 1: Add IPC routing test**

Add a test that verifies:

```ts
// Given incoming text "/status"
// Expect handleIncomingText is not called
// Expect sendImText is called with direct control result
// Expect startOutputForwarding is not called
```

If the current IPC module is too hard to isolate, extract a pure helper:

```ts
export async function handleIncomingRemoteImTextWithControl(input: {
  message: RemoteImIncomingTextMessage
  config: RemoteImConfig
  session: RemoteImSessionInfo | null
  sendControlResult(text: string): Promise<void>
  routeNormalMessage(): Promise<RemoteImRouteResult>
}): Promise<RemoteImRouteResult>
```

Test this helper directly.

- [ ] **Step 2: Run IPC/helper test and confirm failure**

Run:

```bash
npm test -- electron/remote-im/ipc.test.ts electron/remote-im/controlCommands.test.ts electron/remote-im/controlBridge.test.ts
```

Expected: new helper test fails.

- [ ] **Step 3: Implement control branch**

In `remote-im:deliver-incoming-text`:

```ts
const controlCommand = parseRemoteImControlCommand(message.text)
if (controlCommand) {
  const permission = canRouteRemoteImTaskFrom(config, message.fromUserId)
  if (!permission.ok) {
    await sendImText(message.projectId, message.fromUserId, '当前账号无权限执行 AICLI 控制命令。')
    broadcastMessagesChanged(message.projectId)
    return { ok: false as const, error: 'sender-not-allowed' }
  }

  const result = await executeRemoteImControlCommand({
    command: controlCommand,
    session,
    getSourceKind: getRemoteImAicliOutputSourceKind,
    codex: codexControlBridge,
    opencode: opencodeControlBridge
  })
  await sendImText(message.projectId, message.fromUserId, result.message)
  createRemoteImMessage({
    projectId: message.projectId,
    sessionId: session?.sessionId ?? null,
    provider: 'tencent-im',
    remoteMessageId: message.remoteMessageId ?? null,
    fromUserId: null,
    toUserId: message.fromUserId,
    role: 'system',
    direction: 'outgoing',
    content: result.message,
    kind: 'text',
    attachment: null,
    status: 'sent-to-im',
    error: null,
    createdAt: Date.now(),
    sentToAicliAt: null,
    sentToImAt: Date.now()
  })
  broadcastMessagesChanged(message.projectId)
  return { ok: result.ok as boolean }
}
```

Keep audio/image routes unchanged.

- [ ] **Step 4: Verify normal route still starts output forwarding**

Run existing remote IM tests:

```bash
npm test -- electron/remote-im/router.test.ts electron/remote-im/outputForwarding.test.ts electron/remote-im/replyProtocol.test.ts
```

Expected: pass.

- [ ] **Step 5: Commit routing**

```bash
git add electron/remote-im/ipc.ts electron/remote-im/*.test.ts
git commit -m "OPTIMIZE: 分离 IM 控制命令通道"
```

---

## Task 4: OpenCode Source-Level Control

**Files:**
- Modify or create host adapter under `electron/aicli` or `electron/remote-im`
- Modify OpenCode submodule only if generated client is not directly usable
- Test: `electron/remote-im/controlBridge.test.ts` or dedicated OpenCode adapter test

- [ ] **Step 1: Confirm existing OpenCode API can switch agent**

Use source references:

```text
third_party/aicli/opencode/packages/protocol/src/groups/session.ts
third_party/aicli/opencode/packages/server/src/handlers/session.ts
third_party/aicli/opencode/packages/client/src/generated-effect/client.ts
```

The API is:

```ts
session.switchAgent({ sessionID, agent: 'plan' })
session.switchAgent({ sessionID, agent: 'build' })
```

- [ ] **Step 2: Add adapter test**

Test behavior:

```ts
// switchOpenCodeAgent(sessionId, 'plan') calls generated client/session endpoint.
// switchOpenCodeAgent(sessionId, 'build') calls generated client/session endpoint.
// adapter reports "OpenCode session not connected" if no client is attached.
```

- [ ] **Step 3: Implement adapter**

The adapter must use the active OpenCode session/client for the same hosted process, not a global server shared across app instances.

If current host only has PTY access and no OpenCode SDK handle, add a narrow source-level IPC/control bridge in OpenCode custom source. The bridge should be instance-scoped and should expose only:

```ts
status()
switchAgent('plan' | 'build')
model()
usage()
diff()
```

- [ ] **Step 4: Verify OpenCode plan/build**

Run:

```bash
npm test -- electron/remote-im/controlBridge.test.ts
```

Expected: pass.

- [ ] **Step 5: Commit OpenCode adapter**

Commit main repo and submodule if OpenCode source changed:

```bash
git add electron third_party/aicli/opencode
git commit -m "OPTIMIZE: 接入 OpenCode IM 控制命令"
```

---

## Task 5: Codex Source-Level Control

**Files:**
- Modify Codex submodule under `third_party/aicli/codex/codex-rs/tui/src`
- Modify host adapter under `electron/aicli` or `electron/remote-im`
- Test Codex control adapter

- [ ] **Step 1: Locate Codex TUI command handlers**

Start from:

```text
third_party/aicli/codex/codex-rs/tui/src/slash_command.rs
```

Find where `SlashCommand::Status`, `SlashCommand::Plan`, `SlashCommand::Model`, `SlashCommand::Usage`, and `SlashCommand::Diff` are handled.

- [ ] **Step 2: Add source-level control command enum**

Add a narrow enum in Codex source:

```rust
pub enum RemoteImControlCommand {
    Status,
    Plan,
    Build,
    Model,
    Usage,
    Diff,
}
```

Do not include `Stop`.

- [ ] **Step 3: Wire command execution without keyboard input**

The control entrypoint should run on the same TUI app/session state as slash commands. It should return structured text:

```rust
pub struct RemoteImControlResult {
    pub ok: bool,
    pub message: String,
}
```

Map:

```text
Status -> status handler
Plan -> plan mode handler
Build -> new normal/build mode handler
Model -> model status
Usage -> usage handler
Diff -> diff summary
```

If the current turn is busy:

```text
Status/Usage/Diff allowed
Plan/Build returns busy
```

- [ ] **Step 4: Add host adapter**

Expose the Codex control entrypoint to Electron. Use the same instance-scoped process/session as the visible TUI. Do not use environment variables for cross-instance routing.

- [ ] **Step 5: Verify Codex support**

Run Codex unit tests that cover the new control enum and handler:

```bash
cargo test -p codex-tui remote_im_control
```

Then run host tests:

```bash
npm test -- electron/remote-im/controlBridge.test.ts
```

- [ ] **Step 6: Commit Codex changes**

Commit submodule and main repo pointer:

```bash
git -C third_party/aicli/codex status --short
git -C third_party/aicli/codex add .
git -C third_party/aicli/codex commit -m "OPTIMIZE: add remote IM control commands"
git add third_party/aicli/codex electron
git commit -m "OPTIMIZE: 接入 Codex IM 控制命令"
```

---

## Task 6: iOS Slash Command Suggestions

**Files:**
- Modify: `ios/MultiAIIM/MultiAIIM/ChatView.swift`
- Optional Create: `ios/MultiAIIM/MultiAIIM/RemoteIMCommandSuggestion.swift`

- [ ] **Step 1: Add command model**

Create `RemoteIMCommandSuggestion.swift`:

```swift
import Foundation

struct RemoteIMCommandSuggestion: Identifiable, Equatable {
    let command: String
    let title: String
    let detail: String

    var id: String { command }
}

enum RemoteIMCommandSuggestions {
    static let all: [RemoteIMCommandSuggestion] = [
        .init(command: "/status", title: "状态", detail: "查看当前 AICLI 状态"),
        .init(command: "/plan", title: "计划模式", detail: "切换到计划模式"),
        .init(command: "/build", title: "执行模式", detail: "切换到执行模式"),
        .init(command: "/model", title: "模型", detail: "查看当前模型"),
        .init(command: "/usage", title: "用量", detail: "查看当前用量"),
        .init(command: "/diff", title: "改动", detail: "查看当前工作区改动")
    ]

    static func matches(for draft: String) -> [RemoteIMCommandSuggestion] {
        guard draft.hasPrefix("/"), draft.rangeOfCharacter(from: .whitespacesAndNewlines) == nil else { return [] }
        if draft == "/" { return all }
        return all.filter { $0.command.hasPrefix(draft) }
    }
}
```

- [ ] **Step 2: Add tests if iOS test target can access model**

Add XCTest for:

```swift
XCTAssertEqual(RemoteIMCommandSuggestions.matches(for: "/").count, 6)
XCTAssertEqual(RemoteIMCommandSuggestions.matches(for: "/st").first?.command, "/status")
XCTAssertTrue(RemoteIMCommandSuggestions.matches(for: "hello /status").isEmpty)
XCTAssertTrue(RemoteIMCommandSuggestions.matches(for: "/status now").isEmpty)
```

- [ ] **Step 3: Add suggestion panel above input**

In `ChatView.swift`, around the text input area, compute:

```swift
private var commandSuggestions: [RemoteIMCommandSuggestion] {
    RemoteIMCommandSuggestions.matches(for: appState.draftText)
}
```

Render above the input row when non-empty:

```swift
if !commandSuggestions.isEmpty {
    ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 8) {
            ForEach(commandSuggestions) { item in
                Button {
                    appState.draftText = item.command
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(item.command)
                            .font(.system(size: 14, weight: .semibold))
                        Text(item.detail)
                            .font(.system(size: 12))
                            .foregroundStyle(RemoteIMStyle.textSecondary)
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(Color.white, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .stroke(RemoteIMStyle.border, lineWidth: 1)
                    )
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.horizontal, 16)
    }
}
```

- [ ] **Step 4: Verify iOS build**

Run the existing iOS build command used by this repo. If unavailable, at least run:

```bash
xcodebuild -project ios/MultiAIIM/MultiAIIM.xcodeproj -scheme MultiAIIM -configuration Debug -destination 'generic/platform=iOS' build
```

Expected: build succeeds.

- [ ] **Step 5: Commit iOS UI**

```bash
git add ios/MultiAIIM
git commit -m "OPTIMIZE: iOS IM 输入框支持控制命令提示"
```

---

## Task 7: Android Slash Command Suggestions

**Files:**
- Modify: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/MainActivity.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMCommandSuggestion.java`
- Test: `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/RemoteIMCommandSuggestionTest.java`

- [ ] **Step 1: Add command model**

Create `RemoteIMCommandSuggestion.java`:

```java
package com.multiaicode.remoteim;

import java.util.ArrayList;
import java.util.List;

public final class RemoteIMCommandSuggestion {
    public final String command;
    public final String title;
    public final String detail;

    private RemoteIMCommandSuggestion(String command, String title, String detail) {
        this.command = command;
        this.title = title;
        this.detail = detail;
    }

    public static List<RemoteIMCommandSuggestion> all() {
        List<RemoteIMCommandSuggestion> result = new ArrayList<>();
        result.add(new RemoteIMCommandSuggestion("/status", "状态", "查看当前 AICLI 状态"));
        result.add(new RemoteIMCommandSuggestion("/plan", "计划模式", "切换到计划模式"));
        result.add(new RemoteIMCommandSuggestion("/build", "执行模式", "切换到执行模式"));
        result.add(new RemoteIMCommandSuggestion("/model", "模型", "查看当前模型"));
        result.add(new RemoteIMCommandSuggestion("/usage", "用量", "查看当前用量"));
        result.add(new RemoteIMCommandSuggestion("/diff", "改动", "查看当前工作区改动"));
        return result;
    }

    public static List<RemoteIMCommandSuggestion> matches(String draft) {
        List<RemoteIMCommandSuggestion> result = new ArrayList<>();
        if (draft == null || !draft.startsWith("/") || draft.matches(".*\\s+.*")) return result;
        for (RemoteIMCommandSuggestion item : all()) {
            if (draft.equals("/") || item.command.startsWith(draft)) result.add(item);
        }
        return result;
    }
}
```

- [ ] **Step 2: Add unit test**

Create `RemoteIMCommandSuggestionTest.java`:

```java
package com.multiaicode.remoteim;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class RemoteIMCommandSuggestionTest {
    @Test
    public void slashShowsAllCommands() {
        assertEquals(6, RemoteIMCommandSuggestion.matches("/").size());
    }

    @Test
    public void prefixFiltersCommands() {
        assertEquals("/status", RemoteIMCommandSuggestion.matches("/st").get(0).command);
    }

    @Test
    public void normalTextDoesNotShowCommands() {
        assertTrue(RemoteIMCommandSuggestion.matches("hello /status").isEmpty());
        assertTrue(RemoteIMCommandSuggestion.matches("/status now").isEmpty());
    }
}
```

- [ ] **Step 3: Add PopupWindow in MainActivity**

In `MainActivity.java`:

```java
private PopupWindow commandPopup;
```

After `messageInput` creation:

```java
messageInput.addTextChangedListener(new TextWatcher() {
    @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
    @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
        updateCommandSuggestions(s == null ? "" : s.toString());
    }
    @Override public void afterTextChanged(Editable s) {}
});
```

Add:

```java
private void updateCommandSuggestions(String draft) {
    List<RemoteIMCommandSuggestion> suggestions = RemoteIMCommandSuggestion.matches(draft);
    if (suggestions.isEmpty()) {
        dismissCommandPopup();
        return;
    }
    LinearLayout content = new LinearLayout(this);
    content.setOrientation(LinearLayout.VERTICAL);
    content.setBackgroundColor(0xFFFFFFFF);
    for (RemoteIMCommandSuggestion item : suggestions) {
        TextView row = new TextView(this);
        row.setText(item.command + "  " + item.detail);
        row.setTextSize(14);
        row.setTextColor(0xFF0F172A);
        row.setPadding(dp(14), dp(10), dp(14), dp(10));
        row.setOnClickListener(view -> {
            messageInput.setText(item.command);
            messageInput.setSelection(messageInput.getText().length());
            dismissCommandPopup();
        });
        content.addView(row, matchWrap());
    }
    if (commandPopup == null) {
        commandPopup = new PopupWindow(content, dp(280), LinearLayout.LayoutParams.WRAP_CONTENT, false);
        commandPopup.setOutsideTouchable(true);
        commandPopup.setBackgroundDrawable(new ColorDrawable(Color.WHITE));
    } else {
        commandPopup.setContentView(content);
    }
    if (!commandPopup.isShowing()) {
        commandPopup.showAsDropDown(messageInput, 0, -messageInput.getHeight() - dp(48) * suggestions.size());
    }
}

private void dismissCommandPopup() {
    if (commandPopup != null && commandPopup.isShowing()) commandPopup.dismiss();
}
```

Also dismiss popup after `sendText()` succeeds.

- [ ] **Step 4: Run Android tests/build**

Run:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest
./gradlew assembleDebug
```

Expected: both pass.

- [ ] **Step 5: Commit Android UI**

```bash
git add android/MultiAIIM
git commit -m "OPTIMIZE: Android IM 输入框支持控制命令提示"
```

---

## Task 8: End-to-End Verification

**Files:**
- No new files unless test fixes are needed.

- [ ] **Step 1: Verify Desktop tests**

Run:

```bash
npm test -- electron/remote-im/controlCommands.test.ts electron/remote-im/controlBridge.test.ts electron/remote-im/router.test.ts electron/remote-im/outputForwarding.test.ts electron/remote-im/replyProtocol.test.ts
```

Expected: pass.

- [ ] **Step 2: Verify OpenCode/Codex submodule builds**

Run the established commands for the custom bundled binaries:

```bash
npm run build:aicli
```

If the repo uses separate scripts, run the Codex and OpenCode build scripts directly and record exact commands in the final result.

- [ ] **Step 3: Manual verification**

Start Desktop, log into IM, and from a mobile client send:

```text
/status
/plan
/build
请帮我看 /status
```

Expected:

- `/status` returns a direct system status message and does not create a reply marker.
- `/plan` switches mode or returns a busy message.
- `/build` switches mode for OpenCode and for Codex after source support lands.
- `请帮我看 /status` is treated as a normal AICLI task.

- [ ] **Step 4: Commit final integration fixes**

```bash
git status --short
git add <only-files-related-to-this-feature>
git commit -m "OPTIMIZE: 打通 IM AICLI 控制命令"
```

---

## Self-Review Checklist

- Spec coverage:
  - Command channel separated from normal message channel: Tasks 1-3.
  - OpenCode source-level agent switching: Task 4.
  - Codex source-level control: Task 5.
  - Mobile slash suggestions: Tasks 6-7.
  - No `/stop` exposure: Tasks 1-2 tests and design constraints.
- Placeholder scan:
  - No task uses TBD/TODO.
  - Any source paths that still need exact handler location are explicitly discovery steps before implementation.
- Type consistency:
  - Parser returns `RemoteImControlCommand`.
  - Bridge consumes `RemoteImControlCommand`.
  - IPC only branches on parser output and does not start output forwarding for control commands.

