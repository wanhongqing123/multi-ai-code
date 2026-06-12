# Runtime Runner Log Analysis Design

## Background

Multi-AI Code already has a project build pipeline. It can read per-project
build steps, run them in sequence or one at a time, collect build logs, and
send failed-build context to the active AI CLI session for analysis.

The requested workflow is broader than build analysis:

1. The user clicks build.
2. After build, the user clicks run.
3. Multi-AI Code owns the runtime process and captures stdout/stderr.
4. At any time, the user can send the current runtime log to the AI CLI for
   problem analysis.

Build and run have different lifecycles. Build is normally a short task with a
terminal success or failure. Run is commonly a long-lived process that may keep
serving logs until stopped. For that reason, runtime execution must not be mixed
into existing build steps.

## Goals

- Add a per-project runtime configuration next to the existing build
  configuration.
- Add a Run button that starts the configured runtime command.
- Capture runtime stdout/stderr as a separate runtime log.
- Support stopping the running process.
- Allow sending the latest runtime log tail to the currently running AI CLI
  session at any time.
- Keep build state, build logs, runtime state, and runtime logs separate.
- Reuse existing environment concepts where possible:
  - MSYS2
  - Visual Studio Developer Command Prompt
  - output encoding selection

## Non-Goals

- Do not add debugger attach, breakpoints, or step debugging in this first
  iteration.
- Do not mix runtime commands into `build_config.steps`.
- Do not auto-run after build in the first iteration.
- Do not send runtime logs automatically to AI CLI.
- Do not require AI CLI to be running before the runtime process can start.
- Do not make the AI CLI modify code from the runtime-log analysis prompt.

## Recommended Approach

Use an independent Runtime Runner.

The UI may place build and run controls close together because the user workflow
is "build, then run". The backend still keeps them separate:

- `Build Runner`: short-lived build pipeline.
- `Runtime Runner`: long-lived runtime process.

This gives runtime its own configuration, lifecycle, IPC channels, log buffer,
stop behavior, and analysis prompt.

## Data Model

Add `runtime_config` to each project's `project.json`.

```json
{
  "runtime_config": {
    "enabled": true,
    "cwd": ".",
    "command": "npm run dev",
    "envType": "msys",
    "visualStudioInstanceId": "",
    "outputEncoding": "auto"
  }
}
```

### Fields

- `enabled: boolean`
  - Whether this project exposes the Run action.
- `cwd: string`
  - Runtime working directory, resolved relative to `target_repo`.
- `command: string`
  - The command to run.
- `envType: 'msys' | 'visual-studio'`
  - Runtime environment type.
- `visualStudioInstanceId: string`
  - Required only when `envType` is `visual-studio`.
- `outputEncoding: 'auto' | 'utf8' | 'gbk'`
  - Runtime output decoding mode.

The config shape intentionally mirrors a single build step where that makes
sense, but it is stored independently because runtime is not a build step.

## Runtime State

Add a runtime state object analogous to `BuildRuntimeState`, but smaller:

```ts
type RuntimeStatus = 'idle' | 'running' | 'exited' | 'failed' | 'stopped'

interface RuntimeState {
  status: RuntimeStatus
  projectId: string | null
  projectName: string | null
  targetRepo: string | null
  cwd: string | null
  command: string | null
  envType: 'msys' | 'visual-studio' | null
  visualStudioInstanceId: string | null
  visualStudioDisplayName: string | null
  outputEncoding: 'auto' | 'utf8' | 'gbk' | null
  startedAt: string | null
  finishedAt: string | null
  exitCode: number | null
  signal: NodeJS.Signals | null
  log: string
}
```

Only one runtime process is active at a time. If another project already has a
runtime process running, starting a new one should be blocked until the current
runtime process is stopped.

## Main Process Design

Create an Electron main-process runtime module with these responsibilities:

- Validate project and runtime config.
- Resolve `cwd` under `target_repo`.
- Prepare MSYS2 or Visual Studio environment.
- Spawn the runtime command.
- Decode stdout/stderr according to `outputEncoding`.
- Keep an in-memory bounded log buffer.
- Broadcast runtime status and log chunks to renderer windows.
- Stop the child process on request.
- Build a runtime-log analysis prompt from current state.

Suggested files:

- `electron/runtime/config.ts`
- `electron/runtime/runner.ts`
- `electron/runtime/analysisPrompt.ts`
- `electron/runtime/types.ts`

The implementation can reuse patterns from `electron/build/*` rather than
sharing build-specific code directly.

## IPC Design

Expose a new `runtime` API in preload:

```ts
runtime: {
  start(projectId: string): Promise<RuntimeStartResult>
  stop(): Promise<RuntimeStopResult>
  getState(): Promise<RuntimeState>
  getAnalysisPrompt(): Promise<RuntimeAnalysisPromptResult>
  onData(cb: (event: RuntimeDataEvent) => void): () => void
  onStatus(cb: (state: RuntimeState) => void): () => void
}
```

IPC channels:

- `runtime:start`
- `runtime:stop`
- `runtime:get-state`
- `runtime:get-analysis-prompt`
- `runtime:data`
- `runtime:status`

Runtime logs are sent to the AI CLI from the renderer through the existing
`cc.sendUser(sessionId, prompt)` path.

## UI Design

### Project Settings

Add a "Project Runtime" section next to "Project Build":

- Enable runtime
- Environment
- Output encoding
- Visual Studio instance, shown only for Visual Studio mode
- Working directory
- Command

Runtime settings should be disabled while the current project's runtime process
is running.

### Build / Run Panel

Extend the existing build panel into a combined build/run operational surface.
The panel can keep build as the first section and add a separate runtime
section:

- Runtime status
- Start time and finish time
- Run button
- Stop button
- Send Log to AI CLI button
- Runtime command and working directory
- Runtime log viewer

The runtime log must be visually separate from the build log. Users should not
need to infer whether a line came from build or runtime.

### Sending Logs to AI CLI

The Send Log button is enabled when:

- A current project is selected.
- Runtime state belongs to the current project.
- Runtime log is non-empty.
- The main AI CLI session is running.

If the main AI CLI session is not running, show a warning telling the user to
start the main session first.

The action sends a prompt to the active session. It does not start a new AI CLI
session.

## Runtime Analysis Prompt

The runtime analysis prompt should be diagnostic only:

```text
Analyze this runtime log for likely problems.
Do not modify code.
Do not provide patches.
Do not execute commands.

Project: <name> (<id>)
Target repo: <path>
Runtime status: <status>
Environment: <envType>
Working directory: <cwd>
Command: <command>
Exit code: <exitCode or n/a>
Signal: <signal or n/a>

Recent runtime log:
<bounded log tail>

Reply using these sections:
Problem summary
Evidence
Likely cause
What to check first
```

The log tail should be bounded. Reuse the existing build-log limit behavior as a
guide and cap the sent prompt to a safe tail size, for example the latest 200 KB
or an equivalent line-based limit.

## Error Handling

- Missing runtime config: return a structured error and keep runtime state idle.
- Disabled runtime config: block start and explain that runtime is disabled.
- Empty command: block start before spawning.
- Invalid working directory: block start before spawning.
- Missing Visual Studio instance: block start for Visual Studio mode.
- Spawn failure: transition to `failed`, record the error in runtime log, and
  broadcast status.
- Non-zero exit: transition to `failed`.
- User stop: transition to `stopped`.
- Natural zero exit: transition to `exited`.
- AI CLI not running when sending logs: do not discard logs; show a warning.

## Privacy And Safety

- Runtime logs remain local until the user explicitly clicks Send Log.
- No automatic upload or automatic AI CLI submission.
- The analysis prompt includes only project/runtime metadata and the bounded
  runtime log tail.
- Secrets may appear in runtime logs; the first version relies on explicit user
  action before sending. Later work can add redaction before send.

## Testing Plan

Unit tests:

- Runtime config normalization and validation.
- Runtime runner status transitions:
  - idle to running
  - running to exited
  - running to failed
  - running to stopped
- Log buffer truncation.
- Runtime analysis prompt:
  - rejects empty logs
  - includes project/runtime context
  - caps log tail
  - uses diagnostic-only instructions

Renderer tests:

- Project settings render runtime fields and update config.
- Runtime section renders idle, running, failed, stopped, and exited states.
- Run button disables when config is invalid or runtime is already running.
- Send Log button requires a running AI CLI session.

Integration-style tests:

- IPC runtime start returns structured state.
- Runtime data events append to UI state.
- Sending runtime analysis calls `cc.sendUser` with the generated prompt.

## Future Debug Mode

The runtime config leaves room for later debug support without changing the
initial model:

- Add `debugCommand`.
- Add `mode: 'run' | 'debug'`.
- Add debugger-specific launch metadata.
- Add attach instructions or debugger URL capture.

That work should be designed separately after the basic runtime process and log
analysis loop is stable.
