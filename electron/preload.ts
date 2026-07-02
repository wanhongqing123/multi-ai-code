import { contextBridge, ipcRenderer, IpcRendererEvent, webUtils } from 'electron'

export interface ScreenshotOverlayPayload {
  imageDataUrl: string
  logicalSize: { w: number; h: number }
  physicalSize: { w: number; h: number }
}

export interface ScreenshotEditorPayload {
  imageDataUrl: string
  size: { w: number; h: number }
}

export interface ScreenshotDeliverEvent {
  path: string
  prompt: string
}

export interface AiSettings {
  ai_cli: 'claude' | 'codex'
  command?: string
  args?: string[]
  env?: Record<string, string>
}

export interface AppSettings {
  screenshotShortcutEnabled: boolean
  screenshotShortcut: string
}

export type RemoteImContactRelation = 'friend' | 'master' | 'slave'

export interface RemoteImConfig {
  enabled: boolean
  provider: 'tencent-im'
  sdkAppId: number | null
  desktopUserId: string
  desktopRole: 'master' | 'slave'
  userSigMode: 'endpoint' | 'secret-key'
  userSigEndpoint: string
  userSigSecretKey: string
  friendUserIds: string[]
  masterUserIds: string[]
  slaveUserIds: string[]
  allowedUserIds: string[]
  outputFlushIntervalMs: number
  outputMaxChunkChars: number
}

export interface RemoteImAccountConfig {
  provider: 'tencent-im'
  sdkAppId: number | null
  desktopUserId: string
  desktopRole: 'master' | 'slave'
  userSigMode: 'endpoint' | 'secret-key'
  userSigEndpoint: string
  userSigSecretKey: string
  friendUserIds: string[]
  masterUserIds: string[]
  slaveUserIds: string[]
  allowedUserIds: string[]
}

export interface RemoteImLoginState {
  profileId: string | null
  account: RemoteImAccountConfig
}

export type RemoteImConnectionState =
  | 'disabled'
  | 'disconnected'
  | 'connecting'
  | 'connected'
  | 'error'

export interface RemoteImStatus {
  projectId: string | null
  state: RemoteImConnectionState
  detail: string | null
  updatedAt: number
}

export type RemoteImMessageRole = 'remote-user' | 'system' | 'aicli'
export type RemoteImMessageDirection = 'incoming' | 'outgoing' | 'internal'
export type RemoteImMessageKind = 'text' | 'image'
export type RemoteImMessageStatus =
  | 'received'
  | 'rejected'
  | 'sent-to-aicli'
  | 'streaming'
  | 'sent-to-im'
  | 'failed'

export interface RemoteImImageAttachment {
  type: 'image'
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

export type RemoteImMessageAttachment = RemoteImImageAttachment

export interface RemoteImMessage {
  id: number
  projectId: string | null
  sessionId: string | null
  provider: 'tencent-im'
  remoteMessageId: string | null
  fromUserId: string | null
  toUserId: string | null
  role: RemoteImMessageRole
  direction: RemoteImMessageDirection
  content: string
  kind: RemoteImMessageKind
  attachment: RemoteImMessageAttachment | null
  status: RemoteImMessageStatus
  error: string | null
  createdAt: number
  sentToAicliAt: number | null
  sentToImAt: number | null
}

export interface RemoteImIncomingTextMessage {
  projectId: string
  remoteMessageId?: string | null
  fromUserId: string
  toUserId?: string | null
  text: string
  createdAt?: number
}

export interface RemoteImIncomingAudioMessage {
  projectId: string
  remoteMessageId?: string | null
  fromUserId: string
  toUserId?: string | null
  audioUrl: string
  durationSeconds?: number | null
  sizeBytes?: number | null
  uuid?: string | null
  createdAt?: number
}

export interface RemoteImIncomingImageMessage {
  projectId: string
  remoteMessageId?: string | null
  fromUserId: string
  toUserId?: string | null
  imageUrl: string
  thumbnailUrl?: string | null
  width?: number | null
  height?: number | null
  sizeBytes?: number | null
  uuid?: string | null
  fileName?: string | null
  mimeType?: string | null
  createdAt?: number
}

export interface RemoteImSendPeerImageInput {
  fileToken: string
  toUserId?: string | null
  localPath?: string | null
  fileName?: string | null
  mimeType?: string | null
  sizeBytes?: number | null
}

export interface RemoteImRuntimeLogEntryInput {
  projectId?: string | null
  sdkAppId?: number | null
  desktopUserId?: string | null
  peerUserId?: string | null
  messageId?: number | null
  event: string
  detail?: unknown
  createdAt?: number
}

export interface ProjectAiSettingsResponse {
  ok: boolean
  value?: AiSettings
  repaired?: boolean
  error?: string
}

// NOTE: tsconfig.web only includes electron/preload.ts. Importing electron/build/*
// here pulls node-side files into the web program and fails with TS6307, so these
// types intentionally mirror the shared build contracts.
export type BuildStepEnvType = 'system' | 'msys' | 'visual-studio'
export type BuildOutputEncoding = 'auto' | 'utf8' | 'gbk'
export type BuildExecutionScope = 'all' | 'single-step'
export type RuntimeEnvType = 'system' | 'msys' | 'visual-studio'
export type RuntimeOutputEncoding = BuildOutputEncoding
export type RuntimeStatus = 'idle' | 'running' | 'exited' | 'failed' | 'stopped'
export type ScheduledTaskScheduleType = 'once' | 'daily' | 'weekly' | 'interval'
export type ScheduledTaskRunStatus =
  | 'pending'
  | 'queued'
  | 'running'
  | 'succeeded'
  | 'failed'
  | 'timed_out'
  | 'cancelled'
  | 'skipped'

export interface BuildStepConfig {
  id: string
  name: string
  envType: BuildStepEnvType
  cwd: string
  command: string
  enabled: boolean
  visualStudioInstanceId: string
  outputEncoding: BuildOutputEncoding
}

export interface ProjectBuildConfig {
  enabled: boolean
  steps: BuildStepConfig[]
}

export interface ProjectRuntimeConfig {
  enabled: boolean
  cwd: string
  command: string
  envType: RuntimeEnvType
  visualStudioInstanceId: string
  outputEncoding: RuntimeOutputEncoding
}

export interface BuildConfigValidationIssue {
  path: string
  message: string
}

export interface VisualStudioInstallation {
  instanceId: string
  displayName: string
  installationPath: string
  productLineVersion: string | null
  isPrerelease: boolean
}

export type ProjectBuildConfigReadResult =
  | { ok: true; value: ProjectBuildConfig; repaired?: true }
  | { ok: false; error: string }

export type ProjectBuildConfigWriteResult =
  | { ok: true; repaired?: true }
  | { ok: false; error: string; details?: BuildConfigValidationIssue[] }

export type ProjectRuntimeConfigReadResult =
  | { ok: true; value: ProjectRuntimeConfig; repaired?: true }
  | { ok: false; error: string }

export type ProjectRuntimeConfigWriteResult =
  | { ok: true; repaired?: true }
  | { ok: false; error: string; details?: BuildConfigValidationIssue[] }

export interface BuildFailureContext {
  projectId: string
  projectName: string | null
  targetRepo: string
  stepId: string
  stepName: string
  envType: BuildStepEnvType
  visualStudioInstanceId: string | null
  visualStudioDisplayName: string | null
  outputEncoding: BuildOutputEncoding
  cwd: string
  command: string
  exitCode: number | null
  signal: NodeJS.Signals | null
  reason: string
  logTail: string
}

export interface BuildStepRuntime extends BuildStepConfig {
  visualStudioDisplayName: string | null
  status: 'not-run' | 'pending' | 'running' | 'succeeded' | 'failed' | 'skipped'
  resolvedCwd: string | null
  startedAt: string | null
  finishedAt: string | null
  exitCode: number | null
  signal: NodeJS.Signals | null
}

export interface BuildRuntimeState {
  status: 'idle' | 'running' | 'succeeded' | 'failed' | 'stopped'
  scope: BuildExecutionScope | null
  requestedStepId: string | null
  projectId: string | null
  projectName: string | null
  targetRepo: string | null
  startedAt: string | null
  finishedAt: string | null
  activeStepId: string | null
  steps: BuildStepRuntime[]
  log: string
  lastFailure: BuildFailureContext | null
}

export interface BuildDataEvent {
  at: string
  projectId: string | null
  stepId: string | null
  stream: 'stdout' | 'stderr' | 'system'
  chunk: string
}

export type BuildStartResult =
  | { ok: true; state: BuildRuntimeState }
  | { ok: false; error: string; state: BuildRuntimeState }

export type BuildStopResult = { ok: true } | { ok: false; error: string }

export interface BuildStartOptions {
  scope?: BuildExecutionScope
  stepId?: string | null
}

export type BuildFailureAnalysisPromptResult =
  | { ok: true; prompt: string }
  | { ok: false; error: string }

export interface RuntimeState {
  status: RuntimeStatus
  projectId: string | null
  projectName: string | null
  targetRepo: string | null
  cwd: string | null
  command: string | null
  envType: RuntimeEnvType | null
  visualStudioInstanceId: string | null
  visualStudioDisplayName: string | null
  outputEncoding: RuntimeOutputEncoding | null
  startedAt: string | null
  finishedAt: string | null
  exitCode: number | null
  signal: NodeJS.Signals | null
  log: string
}

export interface RuntimeDataEvent {
  at: string
  projectId: string | null
  stream: 'stdout' | 'stderr' | 'system'
  chunk: string
}

export type RuntimeStartResult =
  | { ok: true; state: RuntimeState }
  | { ok: false; error: string; state: RuntimeState }

export type RuntimeStopResult = { ok: true } | { ok: false; error: string }

export type RuntimeAnalysisPromptResult =
  | { ok: true; prompt: string }
  | { ok: false; error: string }

export type RuntimeAnalysisPromptFileResult =
  | { ok: true; filePath: string; message: string }
  | { ok: false; error: string }

export interface ScheduledTaskRun {
  id: number
  taskId: number
  status: ScheduledTaskRunStatus
  scheduledAt: number
  startedAt: number | null
  finishedAt: number | null
  prompt: string
  outputExcerpt: string | null
  error: string | null
  timeoutMinutes: number
}

export interface ScheduledTask {
  id: number
  projectId: string
  targetRepo: string | null
  name: string
  description: string
  goal: string
  instructions: string[]
  enabled: boolean
  scheduleType: ScheduledTaskScheduleType
  scheduleTime: string
  scheduleDays: number[]
  nextRunAt: number | null
  timeoutMinutes: number
  allowCodeChanges: boolean
  allowGitCommit: boolean
  requireTestConfirmation: boolean
  createdAt: number
  updatedAt: number
  lastRun: ScheduledTaskRun | null
}

export interface CreateScheduledTaskInput {
  projectId: string
  name: string
  description: string
  goal: string
  instructions: string[]
  enabled: boolean
  scheduleType: ScheduledTaskScheduleType
  scheduleTime: string
  scheduleDays: number[]
  timeoutMinutes: number
  allowCodeChanges: boolean
  allowGitCommit: boolean
  requireTestConfirmation: boolean
}

export type UpdateScheduledTaskInput = Partial<Omit<CreateScheduledTaskInput, 'projectId'>>

export interface ScheduledTaskQueueState {
  running: {
    taskId: number
    taskName: string
    projectId: string
    targetRepo: string | null
    runId: number
    scheduledAt: number
    prompt: string
  } | null
  waiting: Array<{
    taskId: number
    taskName: string
    projectId: string
    targetRepo: string | null
    runId: number
    scheduledAt: number
    prompt: string
  }>
}

export interface SpawnRequest {
  sessionId: string
  projectId: string
  projectDir: string
  targetRepo: string
  planName?: string
  /** 'none' starts a raw project CLI session without plan prompt injection. */
  planMode?: 'plan' | 'none'
  /** Absolute path resolved via resolvePlanArtifactAbs. */
  planAbsPath?: string
  /** true when plan file does not yet exist on disk. */
  planPending?: boolean
  /** True when this session is allowed to receive scheduled task prompts. */
  allowScheduledTasks?: boolean
  /** First user message to feed after kickoff. */
  initialUserMessage?: string
  /** CLI binary (claude | codex). */
  command: string
  /** CLI args. */
  args: string[]
  env?: Record<string, string>
  cols?: number
  rows?: number
  /**
   * 'new' (default) spawns a fresh CLI session and injects the system prompt.
   * 'resume' rewrites args to the CLI's native continue form (claude
   * --continue / codex resume --last) and skips system-prompt injection so
   * the CLI's own saved conversation history stays clean.
   */
  mode?: 'new' | 'resume'
}

export interface ResumeFailedEvent {
  sessionId: string
  exitCode: number
  signal?: number
  /** Tail of PTY output emitted before exit, for diagnostics in the UI. */
  tail: string
}

export interface DataEvent {
  sessionId: string
  chunk: string
}

export interface ExitEvent {
  sessionId: string
  exitCode: number
  signal?: number
}

export interface StageDoneEvent {
  sessionId: string
  projectId: string
  stageId: number
  raw: string
  params: Record<string, string>
  artifactPath: string | null
  artifactContent: string | null
  /** Path of the snapshot saved to artifacts/history/ (project-dir-relative). */
  snapshotPath?: string | null
}

export interface ArtifactRecord {
  id: number
  project_id: string
  stage_id: number
  path: string
  kind: string
  created_at: string
}

export interface ExternalReviewDecision {
  decision: 'accepted' | 'rejected' | 'needs-human'
  reason: string
  acceptedChanges?: Array<{
    title: string
    reason: string
    fileHint?: string
    lineHint?: string
    recommendation?: string
  }>
  rejectedChanges?: Array<{
    title: string
    reason: string
    fileHint?: string
    lineHint?: string
    recommendation?: string
  }>
  modificationPlan?: string[]
}

export interface JudgeExternalReviewRequest {
  sessionId: string
  planAbsPath: string
  suggestion: {
    rawText: string
    pathHint: string | null
    lineHint: string | null
    linkedDiffFile: { path: string } | null
  }
}


const api = {
  /** Resolve a DataTransfer File to its absolute filesystem path (Electron 32+). */
  getPathForFile: (file: File): string => webUtils.getPathForFile(file),
  clipboard: {
    saveImage: (data: ArrayBuffer, ext: string) =>
      ipcRenderer.invoke('clipboard:save-image', { data, ext }) as Promise<{
        ok: boolean
        path?: string
        error?: string
      }>
  },
  writeTemp: (content: string, ext?: string) =>
    ipcRenderer.invoke('file:write-temp', { content, ext }) as Promise<{
      ok: boolean
      path?: string
      error?: string
    }>,
  saveFileAs: (defaultName: string, content: string) =>
    ipcRenderer.invoke('file:save-as', { defaultName, content }) as Promise<{
      ok: boolean
      canceled?: boolean
      path?: string
    }>,
  search: {
    artifacts: (projectId: string, query: string) =>
      ipcRenderer.invoke('search:artifacts', { projectId, query }) as Promise<{
        ok: boolean
        results: { path: string; stageId: number; line: number; snippet: string }[]
      }>
  },
  dialog: {
    /** Open file picker + read content in ONE round-trip. Does not materialize. */
    pickTextFile: (opts: { title?: string } = {}) =>
      ipcRenderer.invoke('dialog:pick-text-file', opts) as Promise<{
        canceled: boolean
        path?: string
        content?: string
        error?: string
      }>
  },
  doctor: {
    check: () =>
      ipcRenderer.invoke('doctor:check') as Promise<
        Array<{
          name: string
          required: boolean
          ok: boolean
          version?: string
          error?: string
          install: string
        }>
      >
  },
  env: {
    /** Detect MSYS2 / Git-for-Windows bash on the host. */
    detectMsys: () =>
      ipcRenderer.invoke('env:detect-msys') as Promise<{
        available: boolean
        bashPath: string | null
        usrBinDir: string | null
        variant: 'msys2' | 'git' | 'path' | null
        candidates: { path: string; exists: boolean; variant: 'msys2' | 'git' }[]
      }>
  },
  shell: {
    /** Open an MSYS shell window with cwd set to the given dir. */
    openMsysTerminal: (cwd: string) =>
      ipcRenderer.invoke('shell:open-msys-terminal', { cwd }) as Promise<{
        ok: boolean
        variant?: 'msys2' | 'git' | 'path' | null
        error?: string
      }>
  },
  settings: {
    getAppSettings: () =>
      ipcRenderer.invoke('settings:get-app-settings') as Promise<AppSettings>,
    setAppSettings: (settings: AppSettings) =>
      ipcRenderer.invoke('settings:set-app-settings', settings) as Promise<{
        ok: boolean
        value?: AppSettings
        error?: string
      }>
  },
  remoteIm: {
    getConfig: (projectId: string) =>
      ipcRenderer.invoke('remote-im:get-config', { projectId }) as Promise<
        { ok: true; value: RemoteImConfig } | { ok: false; error: string }
      >,
    getLoginState: () =>
      ipcRenderer.invoke('remote-im:get-login-state') as Promise<
        { ok: true; value: RemoteImLoginState } | { ok: false; error: string }
      >,
    getAccountByUserId: (userId: string) =>
      ipcRenderer.invoke('remote-im:get-account-by-user-id', { userId }) as Promise<
        { ok: true; value: RemoteImLoginState | null } | { ok: false; error: string }
      >,
    setAccount: (account: RemoteImAccountConfig) =>
      ipcRenderer.invoke('remote-im:set-account', { account }) as Promise<
        { ok: true; value: RemoteImLoginState } | { ok: false; error: string }
      >,
    setConfig: (projectId: string, config: RemoteImConfig) =>
      ipcRenderer.invoke('remote-im:set-config', { projectId, config }) as Promise<
        | { ok: true; value: RemoteImConfig; repaired?: true }
        | { ok: false; error: string; details?: Array<{ path: string; message: string }> }
      >,
    getStatus: (projectId: string) =>
      ipcRenderer.invoke('remote-im:get-status', { projectId }) as Promise<RemoteImStatus>,
    listMessages: (projectId: string, limit = 100) =>
      ipcRenderer.invoke('remote-im:list-messages', { projectId, limit }) as Promise<
        RemoteImMessage[]
      >,
    clearMessages: (projectId: string) =>
      ipcRenderer.invoke('remote-im:clear-messages', { projectId }) as Promise<{ ok: true }>,
    deleteContact: (projectId: string, userId: string) =>
      ipcRenderer.invoke('remote-im:delete-contact', { projectId, userId }) as Promise<
        | { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState }
        | { ok: false; error: string }
      >,
    sendLocalMessage: (projectId: string, text: string) =>
      ipcRenderer.invoke('remote-im:send-local-message', { projectId, text }) as Promise<
        { ok: boolean; error?: string }
      >,
    sendPeerMessage: (projectId: string, text: string, toUserId?: string | null) =>
      ipcRenderer.invoke('remote-im:send-peer-message', { projectId, text, toUserId }) as Promise<
        { ok: boolean; error?: string; toUserId?: string }
      >,
    sendPeerImage: (projectId: string, image: RemoteImSendPeerImageInput) =>
      ipcRenderer.invoke('remote-im:send-peer-image', { projectId, ...image }) as Promise<
        { ok: boolean; error?: string; toUserId?: string }
      >,
    deliverIncomingText: (message: RemoteImIncomingTextMessage) =>
      ipcRenderer.invoke('remote-im:deliver-incoming-text', message) as Promise<{
        ok: boolean
        error?: string
      }>,
    deliverIncomingAudio: (message: RemoteImIncomingAudioMessage) =>
      ipcRenderer.invoke('remote-im:deliver-incoming-audio', message) as Promise<{
        ok: boolean
        error?: string
      }>,
    deliverIncomingImage: (message: RemoteImIncomingImageMessage) =>
      ipcRenderer.invoke('remote-im:deliver-incoming-image', message) as Promise<{
        ok: boolean
        error?: string
      }>,
    updateSdkStatus: (status: Pick<RemoteImStatus, 'projectId' | 'state' | 'detail'>) =>
      ipcRenderer.invoke('remote-im:update-sdk-status', status) as Promise<{ ok: true }>,
    markOutgoingMessageSent: (projectId: string, messageId: number) =>
      ipcRenderer.invoke('remote-im:mark-outgoing-message-sent', {
        projectId,
        messageId
      }) as Promise<{ ok: true }>,
    markOutgoingMessageFailed: (projectId: string, messageId: number, error: string) =>
      ipcRenderer.invoke('remote-im:mark-outgoing-message-failed', {
        projectId,
        messageId,
        error
      }) as Promise<{ ok: true }>,
    writeRuntimeLog: (entry: RemoteImRuntimeLogEntryInput) =>
      ipcRenderer.invoke('remote-im:write-runtime-log', { entry }) as Promise<
        { ok: true } | { ok: false; error: string }
      >,
    onStatus: (cb: (status: RemoteImStatus) => void) => {
      const handler = (_event: IpcRendererEvent, status: RemoteImStatus) => cb(status)
      ipcRenderer.on('remote-im:status', handler)
      return () => ipcRenderer.removeListener('remote-im:status', handler)
    },
    onMessagesChanged: (cb: (evt: { projectId: string | null }) => void) => {
      const handler = (_event: IpcRendererEvent, evt: { projectId: string | null }) => cb(evt)
      ipcRenderer.on('remote-im:messages-changed', handler)
      return () => ipcRenderer.removeListener('remote-im:messages-changed', handler)
    },
    onOutgoingText: (
      cb: (evt: { projectId: string; toUserId: string; text: string; messageId?: number | null }) => void
    ) => {
      const handler = (
        _event: IpcRendererEvent,
        evt: { projectId: string; toUserId: string; text: string; messageId?: number | null }
      ) => cb(evt)
      ipcRenderer.on('remote-im:outgoing-text', handler)
      return () => ipcRenderer.removeListener('remote-im:outgoing-text', handler)
    },
    onOutgoingImage: (
      cb: (evt: { projectId: string; toUserId: string; fileToken: string; messageId?: number | null }) => void
    ) => {
      const handler = (
        _event: IpcRendererEvent,
        evt: { projectId: string; toUserId: string; fileToken: string; messageId?: number | null }
      ) => cb(evt)
      ipcRenderer.on('remote-im:outgoing-image', handler)
      return () => ipcRenderer.removeListener('remote-im:outgoing-image', handler)
    }
  },
  git: {
    status: (cwd: string) =>
      ipcRenderer.invoke('git:status', { cwd }) as Promise<{
        ok: boolean
        branch?: string
        files?: { status: string; path: string }[]
        error?: string
      }>,
    commit: (cwd: string, message: string) =>
      ipcRenderer.invoke('git:commit', { cwd, message }) as Promise<{
        ok: boolean
        output?: string
        error?: string
      }>,
    checkoutBranch: (cwd: string, name: string) =>
      ipcRenderer.invoke('git:checkout-branch', { cwd, name }) as Promise<{
        ok: boolean
        created?: boolean
        error?: string
      }>,
    /** List recent commits on the given repo. */
    log: (cwd: string, limit?: number) =>
      ipcRenderer.invoke('git:log', { cwd, limit }) as Promise<{
        ok: boolean
        entries?: {
          hash: string
          short: string
          author: string
          date: string
          subject: string
        }[]
        error?: string
      }>,
    /** Unified diff between various sources:
     *   - working: uncommitted changes vs HEAD
     *   - head1:   HEAD~1..HEAD (latest commit)
     *   - commit:  show a single commit (refs=[hash])
     *   - range:   A..B diff (refs=[from, to]) */
    diff: (
      cwd: string,
      mode: 'working' | 'head1' | 'commit' | 'range',
      refs?: string[]
    ) =>
      ipcRenderer.invoke('git:diff', { cwd, mode, refs }) as Promise<{
        ok: boolean
        diff?: string
        error?: string
      }>
  },
  ping: () => ipcRenderer.invoke('app:ping'),
  version: () => ipcRenderer.invoke('app:version'),
  project: {
    list: () =>
      ipcRenderer.invoke('project:list') as Promise<
        Array<{
          id: string
          name: string
          target_repo: string
          dir: string
          created_at: string
          updated_at: string
        }>
      >,
    create: (name: string, target_repo: string) =>
      ipcRenderer.invoke('project:create', { name, target_repo }) as Promise<{
        ok: boolean
        id?: string
        name?: string
        target_repo?: string
        dir?: string
        error?: string
      }>,
    delete: (id: string) =>
      ipcRenderer.invoke('project:delete', { id }) as Promise<{
        ok: boolean
        trashPath?: string
        snapshot?: { id: string; name: string; target_repo: string } | null
        error?: string
      }>,
    undelete: (trashPath: string, snapshot: { id: string; name: string; target_repo: string }) =>
      ipcRenderer.invoke('project:undelete', { trashPath, snapshot }) as Promise<{
        ok: boolean
        error?: string
      }>,
    purgeTrash: (trashPath: string) =>
      ipcRenderer.invoke('project:purge-trash', { trashPath }) as Promise<{
        ok: boolean
      }>,
    rename: (id: string, name: string) =>
      ipcRenderer.invoke('project:rename', { id, name }) as Promise<{
        ok: boolean
        error?: string
      }>,
    touch: (id: string) => ipcRenderer.invoke('project:touch', { id }),
    getStageConfigs: (id: string) =>
      ipcRenderer.invoke('project:get-stage-configs', { id }) as Promise<
        Record<string, { command?: string; args?: string[]; env?: Record<string, string> }>
      >,
    getMsysEnabled: (id: string) =>
      ipcRenderer.invoke('project:get-msys-enabled', { id }) as Promise<boolean>,
    setMsysEnabled: (id: string, enabled: boolean) =>
      ipcRenderer.invoke('project:set-msys-enabled', { id, enabled }) as Promise<{
        ok: boolean
      }>,
    setStageConfigs: (
      id: string,
      configs: Record<
        string,
        { command?: string; args?: string[]; env?: Record<string, string> }
      >
    ) =>
      ipcRenderer.invoke('project:set-stage-configs', { id, configs }) as Promise<{ ok: boolean }>,
    getBuildConfig: (id: string) =>
      ipcRenderer.invoke('project:get-build-config', { id }) as Promise<ProjectBuildConfigReadResult>,
    getRuntimeConfig: (id: string) =>
      ipcRenderer.invoke('project:get-runtime-config', { id }) as Promise<ProjectRuntimeConfigReadResult>,
    listVisualStudioInstallations: () =>
      ipcRenderer.invoke('project:list-visual-studio-installations') as Promise<
        { ok: true; value: VisualStudioInstallation[] } | { ok: false; error: string }
      >,
    setBuildConfig: (id: string, config: ProjectBuildConfig) =>
      ipcRenderer.invoke('project:set-build-config', {
        id,
        config
      }) as Promise<ProjectBuildConfigWriteResult>,
    setRuntimeConfig: (id: string, config: ProjectRuntimeConfig) =>
      ipcRenderer.invoke('project:set-runtime-config', {
        id,
        config
      }) as Promise<ProjectRuntimeConfigWriteResult>,
    getAiSettings: (id: string) =>
      ipcRenderer.invoke('project:get-ai-settings', { id }) as Promise<ProjectAiSettingsResponse>,
    setAiSettings: (id: string, settings: AiSettings) =>
      ipcRenderer.invoke('project:set-ai-settings', {
        id,
        settings
      }) as Promise<{ ok: boolean; repaired?: boolean; error?: string }>,
    getRepoViewAiSettings: (id: string) =>
      ipcRenderer.invoke(
        'project:get-repo-view-ai-settings',
        { id }
      ) as Promise<ProjectAiSettingsResponse>,
    setRepoViewAiSettings: (id: string, settings: AiSettings) =>
      ipcRenderer.invoke('project:set-repo-view-ai-settings', {
        id,
        settings
      }) as Promise<{ ok: boolean; repaired?: boolean; error?: string }>,
    pickDir: () =>
      ipcRenderer.invoke('project:pick-dir') as Promise<{
        canceled: boolean
        path?: string
      }>,
    setTargetRepo: (id: string, path: string) =>
      ipcRenderer.invoke('project:set-target-repo', { id, path }) as Promise<{
        ok: boolean
        target_repo?: string
        name?: string
        error?: string
      }>
  },

  build: {
    start: (projectId: string, options?: BuildStartOptions) =>
      ipcRenderer.invoke('build:start', {
        id: projectId,
        scope: options?.scope ?? 'all',
        stepId: options?.stepId ?? null
      }) as Promise<BuildStartResult>,
    stop: () => ipcRenderer.invoke('build:stop') as Promise<BuildStopResult>,
    getState: () => ipcRenderer.invoke('build:get-state') as Promise<BuildRuntimeState>,
    getFailureAnalysisPrompt: () =>
      ipcRenderer.invoke('build:get-failure-analysis-prompt') as Promise<BuildFailureAnalysisPromptResult>,
    onData: (cb: (evt: BuildDataEvent) => void) => {
      const handler = (_event: IpcRendererEvent, evt: BuildDataEvent) => cb(evt)
      ipcRenderer.on('build:data', handler)
      return () => ipcRenderer.removeListener('build:data', handler)
    },
    onStatus: (cb: (state: BuildRuntimeState) => void) => {
      const handler = (_event: IpcRendererEvent, state: BuildRuntimeState) => cb(state)
      ipcRenderer.on('build:status', handler)
      return () => ipcRenderer.removeListener('build:status', handler)
    }
  },

  runtime: {
    start: (projectId: string) =>
      ipcRenderer.invoke('runtime:start', {
        id: projectId
      }) as Promise<RuntimeStartResult>,
    stop: () => ipcRenderer.invoke('runtime:stop') as Promise<RuntimeStopResult>,
    getState: () => ipcRenderer.invoke('runtime:get-state') as Promise<RuntimeState>,
    getAnalysisPrompt: () =>
      ipcRenderer.invoke('runtime:get-analysis-prompt') as Promise<RuntimeAnalysisPromptResult>,
    getAnalysisPromptFile: () =>
      ipcRenderer.invoke('runtime:get-analysis-prompt-file') as Promise<RuntimeAnalysisPromptFileResult>,
    onData: (cb: (evt: RuntimeDataEvent) => void) => {
      const handler = (_event: IpcRendererEvent, evt: RuntimeDataEvent) => cb(evt)
      ipcRenderer.on('runtime:data', handler)
      return () => ipcRenderer.removeListener('runtime:data', handler)
    },
    onStatus: (cb: (state: RuntimeState) => void) => {
      const handler = (_event: IpcRendererEvent, state: RuntimeState) => cb(state)
      ipcRenderer.on('runtime:status', handler)
      return () => ipcRenderer.removeListener('runtime:status', handler)
    }
  },

  scheduledTasks: {
    list: (projectId: string) =>
      ipcRenderer.invoke('scheduled-tasks:list', { projectId }) as Promise<
        ScheduledTask[]
      >,
    create: (input: CreateScheduledTaskInput) =>
      ipcRenderer.invoke('scheduled-tasks:create', input) as Promise<
        { ok: true; task: ScheduledTask } | { ok: false; error: string }
      >,
    update: (id: number, patch: UpdateScheduledTaskInput) =>
      ipcRenderer.invoke('scheduled-tasks:update', { id, patch }) as Promise<
        { ok: true; task: ScheduledTask | null } | { ok: false; error: string }
      >,
    delete: (id: number) =>
      ipcRenderer.invoke('scheduled-tasks:delete', { id }) as Promise<
        { ok: true } | { ok: false; error: string }
      >,
    setEnabled: (id: number, enabled: boolean) =>
      ipcRenderer.invoke('scheduled-tasks:set-enabled', { id, enabled }) as Promise<
        { ok: true; task: ScheduledTask | null } | { ok: false; error: string }
      >,
    runNow: (req: { taskId: number; sessionId?: string | null; targetRepo?: string | null }) =>
      ipcRenderer.invoke('scheduled-tasks:run-now', req) as Promise<
        | {
            ok: true
            delivery: 'sent' | 'queued' | 'failed'
            queued: boolean
            state: ScheduledTaskQueueState
          }
        | { ok: false; error: string; state?: ScheduledTaskQueueState }
      >,
    scanNow: (projectId?: string) =>
      ipcRenderer.invoke('scheduled-tasks:scan-now', { projectId }) as Promise<
        { ok: true; state: ScheduledTaskQueueState } | { ok: false; error: string }
      >,
    queueState: () =>
      ipcRenderer.invoke('scheduled-tasks:queue-state') as Promise<ScheduledTaskQueueState>,
    cancelQueueRun: (runId?: number | null) =>
      ipcRenderer.invoke('scheduled-tasks:cancel-queue-run', { runId }) as Promise<{
        ok: true
        cancelled: boolean
        state: ScheduledTaskQueueState
      }>
  },

  repoView: {
    openWindow: (projectId: string) =>
      ipcRenderer.invoke('repo-view:open-window', { projectId }) as Promise<{
        ok: boolean
        error?: string
      }>,
    listTree: (root: string, dir = '') =>
      ipcRenderer.invoke('repo-view:list-tree', { root, dir }) as Promise<{
        ok: boolean
        entries: Array<{ name: string; path: string; isDirectory: boolean }>
        error?: string
      }>,
    readFile: (root: string, path: string) =>
      ipcRenderer.invoke('repo-view:read-file', { root, path }) as Promise<{
        ok: boolean
        content?: string
        byteLength?: number
        error?: string
      }>,
    memoryLoad: (root: string) =>
      ipcRenderer.invoke('repo-view:memory-load', { root }) as Promise<{
        ok: boolean
        summary?: string
        recentTopics?: unknown[]
        error?: string
      }>,
    memoryFileNote: (root: string, path: string) =>
      ipcRenderer.invoke('repo-view:memory-file-note', { root, path }) as Promise<{
        ok: boolean
        fileNote?: string
        error?: string
      }>,
    memoryApply: (root: string, path: string, memoryUpdate: string) =>
      ipcRenderer.invoke('repo-view:memory-apply', { root, path, memoryUpdate }) as Promise<{
        ok: boolean
        summary?: string
        fileNote?: string
        recentTopics?: unknown[]
        error?: string
      }>,
    historyLoad: (root: string) =>
      ipcRenderer.invoke('repo-view:history-load', { root }) as Promise<{
        ok: boolean
        messages?: Array<{ id: string; role: 'user' | 'assistant'; text: string }>
        error?: string
      }>,
    historySave: (
      root: string,
      messages: Array<{ id: string; role: 'user' | 'assistant'; text: string }>
    ) =>
      ipcRenderer.invoke('repo-view:history-save', { root, messages }) as Promise<{
        ok: boolean
        messages?: Array<{ id: string; role: 'user' | 'assistant'; text: string }>
        error?: string
      }>,
    analysisStart: (req: {
      projectId: string
      targetRepo: string
      command: string
      args: string[]
      env?: Record<string, string>
    }) =>
      ipcRenderer.invoke('repo-view:analysis-start', req) as Promise<{
        ok: boolean
        error?: string
      }>,
    analysisSend: (req: { repoRoot: string; text: string }) =>
      ipcRenderer.invoke('repo-view:analysis-send', req) as Promise<{
        ok: boolean
        error?: string
      }>,
    analysisStop: () =>
      ipcRenderer.invoke('repo-view:analysis-stop') as Promise<{
        ok: boolean
        error?: string
      }>,
    analysisHas: () =>
      ipcRenderer.invoke('repo-view:analysis-has') as Promise<{
        ok: boolean
        running?: boolean
        error?: string
      }>,
    analysisInput: (data: string) =>
      ipcRenderer.send('repo-view:analysis-input', { data }),
    analysisPaste: (data: string) =>
      ipcRenderer.invoke('repo-view:analysis-paste', { data }) as Promise<{
        ok: boolean
        error?: string
      }>,
    analysisResize: (cols: number, rows: number) =>
      ipcRenderer.send('repo-view:analysis-resize', { cols, rows }),
    onAnalysisData: (cb: (evt: { chunk: string }) => void) => {
      const handler = (_: IpcRendererEvent, evt: { chunk: string }) => cb(evt)
      ipcRenderer.on('repo-view:analysis-data', handler)
      return () => ipcRenderer.removeListener('repo-view:analysis-data', handler)
    },
    onAnalysisStatus: (
      cb: (evt: { status: string; exitCode?: number; signal?: number }) => void
    ) => {
      const handler = (
        _: IpcRendererEvent,
        evt: { status: string; exitCode?: number; signal?: number }
      ) => cb(evt)
      ipcRenderer.on('repo-view:analysis-status', handler)
      return () => ipcRenderer.removeListener('repo-view:analysis-status', handler)
    }
  },

  fs: {
    readUtf8: (path: string) =>
      ipcRenderer.invoke('fs:read-utf8', { path }) as Promise<
        { ok: true; content: string } | { ok: false; error: string }
      >
  },

  cc: {
    spawn: (opts: SpawnRequest) =>
      ipcRenderer.invoke('cc:spawn', opts) as Promise<{ ok: boolean; error?: string }>,
    write: (sessionId: string, data: string) =>
      ipcRenderer.send('cc:input', { sessionId, data }),
    paste: (sessionId: string, data: string) =>
      ipcRenderer.invoke('cc:paste', { sessionId, data }) as Promise<{
        ok: boolean
        error?: string
      }>,
    resize: (sessionId: string, cols: number, rows: number) =>
      ipcRenderer.send('cc:resize', { sessionId, cols, rows }),
    kill: (sessionId: string) =>
      ipcRenderer.invoke('cc:kill', { sessionId }) as Promise<{ ok: boolean; error?: string }>,
    sendUser: (sessionId: string, text: string) =>
      ipcRenderer.invoke('cc:send-user', { sessionId, text }) as Promise<{
        ok: boolean
        error?: string
      }>,
    judgeExternalReview: (req: JudgeExternalReviewRequest) =>
      ipcRenderer.invoke('cc:judge-external-review', req) as Promise<
        | { ok: true; result: ExternalReviewDecision }
        | { ok: false; error: string }
      >,
    killAll: () =>
      ipcRenderer.invoke('cc:kill-all') as Promise<{ ok: boolean; killed: string[] }>,
    list: () => ipcRenderer.invoke('cc:list') as Promise<string[]>,
    has: (sessionId: string) =>
      ipcRenderer.invoke('cc:has', { sessionId }) as Promise<boolean>,
    onData: (cb: (evt: DataEvent) => void): (() => void) => {
      const handler = (_e: IpcRendererEvent, evt: DataEvent) => cb(evt)
      ipcRenderer.on('cc:data', handler)
      return () => ipcRenderer.removeListener('cc:data', handler)
    },
    onExit: (cb: (evt: ExitEvent) => void): (() => void) => {
      const handler = (_e: IpcRendererEvent, evt: ExitEvent) => cb(evt)
      ipcRenderer.on('cc:exit', handler)
      return () => ipcRenderer.removeListener('cc:exit', handler)
    },
    onResumeFailed: (cb: (evt: ResumeFailedEvent) => void): (() => void) => {
      const handler = (_e: IpcRendererEvent, evt: ResumeFailedEvent) => cb(evt)
      ipcRenderer.on('cc:resume-failed', handler)
      return () => ipcRenderer.removeListener('cc:resume-failed', handler)
    },
    onNotice: (
      cb: (evt: {
        sessionId: string
        level: 'info' | 'warn' | 'error'
        message: string
      }) => void
    ): (() => void) => {
      const handler = (_e: IpcRendererEvent, evt: any) => cb(evt)
      ipcRenderer.on('cc:notice', handler)
      return () => ipcRenderer.removeListener('cc:notice', handler)
    }
  },

  stage: {
    triggerDone: (req: {
      sessionId: string
      projectId: string
      stageId: number
      projectDir: string
      artifactPath?: string
      verdict?: string
      summary?: string
    }) =>
      ipcRenderer.invoke('stage:trigger-done', req) as Promise<{
        ok: boolean
        artifactFound?: boolean
        snapshotPath?: string | null
        error?: string
      }>
  },
  artifact: {
    list: (projectId: string, stageId?: number) =>
      ipcRenderer.invoke('artifact:list', { projectId, stageId }) as Promise<
        ArtifactRecord[]
      >,
    read: (projectDir: string, path: string) =>
      ipcRenderer.invoke('artifact:read', { projectDir, path }) as Promise<{
        ok: boolean
        content?: string
        error?: string
      }>,
    readCurrent: (projectDir: string, stageId: number, label?: string) =>
      ipcRenderer.invoke('artifact:read-current', {
        projectDir,
        stageId,
        label
      }) as Promise<{
        ok: boolean
        path?: string
        relPath?: string
        content?: string
        error?: string
      }>,
    restore: (req: {
      projectId: string
      projectDir: string
      stageId: number
      snapshotPath: string
      sessionId?: string
      label?: string
    }) =>
      ipcRenderer.invoke('artifact:restore', req) as Promise<{
        ok: boolean
        artifactPath?: string
        snapshotPath?: string | null
        error?: string
      }>,
    seed: (req: {
      projectId: string
      projectDir: string
      stageId: number
      snapshotPath?: string
      pickFile?: boolean
      label?: string
    }) =>
      ipcRenderer.invoke('artifact:seed', req) as Promise<{
        ok: boolean
        canceled?: boolean
        artifactPath?: string
        artifactAbs?: string
        snapshotPath?: string | null
        sourceLabel?: string
        error?: string
      }>,
    importFile: (req: {
      projectId: string
      projectDir: string
      stageId: number
      sessionId?: string
      label?: string
    }) =>
      ipcRenderer.invoke('artifact:import-file', req) as Promise<{
        ok: boolean
        canceled?: boolean
        artifactPath?: string
        snapshotPath?: string | null
        error?: string
      }>,
    /** Write user-confirmed external content as the stage artifact + broadcast stage:done. */
    commitContent: (req: {
      projectId: string
      projectDir: string
      stageId: number
      content: string
      sourcePath: string
      sessionId?: string
      label?: string
    }) =>
      ipcRenderer.invoke('artifact:commit-content', req) as Promise<{
        ok: boolean
        artifactPath?: string
        artifactAbs?: string
        snapshotPath?: string | null
        error?: string
      }>
  },
  plan: {
    list: (projectDir: string) =>
      ipcRenderer.invoke('plan:list', { projectDir }) as Promise<{
        ok: boolean
        items: {
          name: string
          abs: string
          source: 'internal' | 'external'
          description?: string
          details?: string
        }[]
        error?: string
      }>,
    createInternal: (req: { projectDir: string; name: string }) =>
      ipcRenderer.invoke('plan:createInternal', req) as Promise<
        | { ok: true; name: string; abs: string }
        | { ok: false; error: string }
      >,
    registerExternal: (req: { projectDir: string; externalPath: string }) =>
      ipcRenderer.invoke('plan:registerExternal', req) as Promise<
        | { ok: true; name: string }
        | { ok: false; error: string }
      >,
    updateDescription: (req: {
      projectDir: string
      name: string
      description: string
      details?: string
    }) =>
      ipcRenderer.invoke('plan:updateDescription', req) as Promise<
        { ok: true } | { ok: false; error: string }
      >,
    updateMetadata: (req: {
      projectDir: string
      name: string
      description: string
      details: string
    }) =>
      ipcRenderer.invoke('plan:updateMetadata', req) as Promise<
        { ok: true } | { ok: false; error: string }
      >,
    removeExternal: (req: { projectDir: string; name: string }) =>
      ipcRenderer.invoke('plan:removeExternal', req) as Promise<
        { ok: true } | { ok: false; error: string }
      >
  },
  screenshot: {
    start: () =>
      ipcRenderer.invoke('screenshot:start') as Promise<{ ok: boolean }>,
    overlayLoadPayload: (token: string) =>
      ipcRenderer.invoke('screenshot:overlay-load', token) as Promise<
        | { ok: true; payload: ScreenshotOverlayPayload }
        | { ok: false; error: string }
      >,
    overlayCommit: (req: {
      token: string
      logicalRect: { x: number; y: number; w: number; h: number }
      logicalSize: { w: number; h: number }
      physicalSize: { w: number; h: number }
    }) =>
      ipcRenderer.invoke('screenshot:overlay-commit', req) as Promise<
        { ok: true } | { ok: false; error: string }
      >,
    overlayCancel: (token: string) =>
      ipcRenderer.invoke('screenshot:overlay-cancel', token) as Promise<{ ok: boolean }>,
    editorLoadPayload: (token: string) =>
      ipcRenderer.invoke('screenshot:editor-load', token) as Promise<
        | { ok: true; payload: ScreenshotEditorPayload }
        | { ok: false; error: string }
      >,
    editorCancel: (token: string) =>
      ipcRenderer.invoke('screenshot:editor-cancel', token) as Promise<{ ok: boolean }>,
    editorSend: (req: {
      token: string
      pngBytes?: Uint8Array | null
      useOriginal?: boolean
      prompt: string
    }) =>
      ipcRenderer.invoke('screenshot:editor-send', req) as Promise<
        { ok: true; path: string } | { ok: false; error: string }
      >,
    onDeliver: (cb: (evt: ScreenshotDeliverEvent) => void): (() => void) => {
      const handler = (_e: IpcRendererEvent, evt: ScreenshotDeliverEvent) =>
        cb(evt)
      ipcRenderer.on('screenshot:deliver', handler)
      return () => ipcRenderer.removeListener('screenshot:deliver', handler)
    },
    onError: (
      cb: (evt: { message: string }) => void
    ): (() => void) => {
      const handler = (_e: IpcRendererEvent, evt: { message: string }) => cb(evt)
      ipcRenderer.on('screenshot:error', handler)
      return () => ipcRenderer.removeListener('screenshot:error', handler)
    }
  },
  habit: {
    settings: {
      get: () => ipcRenderer.invoke('habit:settings:get') as Promise<unknown>,
      save: (next: unknown) =>
        ipcRenderer.invoke('habit:settings:save', next) as Promise<{ ok: boolean }>,
      update: (patch: unknown) =>
        ipcRenderer.invoke('habit:settings:update', patch) as Promise<unknown>
    },
    flows: {
      list: (opts?: { statuses?: Array<'candidate' | 'active' | 'disabled'>; limit?: number }) =>
        ipcRenderer.invoke('habit:flows:list', opts) as Promise<
          Array<{
            id: number
            kind: 'app-flow' | 'ui-adjustment'
            title: string
            summary: string
            evidence_count: number
            risk_level: 'low' | 'high'
            enabled_by_default: number
            status: 'candidate' | 'active' | 'disabled'
            payload: string
            created_at: number
            updated_at: number
          }>
        >,
      updateStatus: (req: { id: number; status: 'candidate' | 'active' | 'disabled' }) =>
        ipcRenderer.invoke('habit:flows:update-status', req) as Promise<{ ok: boolean }>,
      clear: () =>
        ipcRenderer.invoke('habit:flows:clear') as Promise<{
          ok: boolean
          removed: number
        }>
    },
    events: {
      recent: (limit?: number) =>
        ipcRenderer.invoke('habit:events:recent', { limit }) as Promise<{
          events: Array<{
            id: number
            ts: number
            kind: string
            payload: string
            source: 'app_ui' | null
            project_id: string | null
            repo_path: string | null
            source_window: string | null
          }>
          total: number
        }>,
      clear: () =>
        ipcRenderer.invoke('habit:events:clear') as Promise<{ ok: boolean; removed: number }>
    },
    screenSampler: {
      state: () =>
        ipcRenderer.invoke('habit:screen-sampler:state') as Promise<{
          enabled: boolean
          paused: boolean
          runtime: {
            running: boolean
            lastL1At: number
            lastL2At: number
            lastError: { label: string; message: string; at: number } | null
            lastWindowTitle: string | null
            lastWindowApp: string | null
          } | null
          activeWinLoadError: string | null
        }>,
      togglePause: () =>
        ipcRenderer.invoke('habit:screen-sampler:toggle-pause') as Promise<{
          paused: boolean
        }>,
      setPaused: (paused: boolean) =>
        ipcRenderer.invoke('habit:screen-sampler:set-paused', { paused }) as Promise<{
          paused: boolean
        }>
    },
    runNow: () =>
      ipcRenderer.invoke('habit:run-now') as Promise<
        | {
            ok: true
            outcome: {
              ran: boolean
              reason: string
              clustersFound?: number
              flowsGenerated?: number
              candidatesInserted?: number
              startedAt: number
              finishedAt: number
            }
          }
        | { ok: false; error: string }
      >,
    candidates: {
      list: (opts?: { statuses?: string[]; limit?: number }) =>
        ipcRenderer.invoke('habit:candidates:list', opts) as Promise<
          Array<{
            id: number
            created_at: number
            cluster_kind: string
            cluster_size: number
            source_event_ids: string
            representative_samples: string
            generated_title: string | null
            generated_body: string | null
            generated_meta: string | null
            status: string
            reviewed_at: number | null
            snoozed_until: number | null
            error_message: string | null
          }>
        >,
      updateStatus: (req: {
        id: number
        status: string
        snoozedUntil?: number | null
        errorMessage?: string | null
      }) =>
        ipcRenderer.invoke('habit:candidates:update-status', req) as Promise<{ ok: boolean }>,
      clear: () =>
        ipcRenderer.invoke('habit:candidates:clear') as Promise<{
          ok: boolean
          removed: number
        }>,
      acceptAsSkill: (req: {
        candidateId: number
        name: string
        description?: string | null
        trigger?: string | null
        steps: unknown[]
      }) =>
        ipcRenderer.invoke('habit:candidates:accept-as-skill', req) as Promise<{
          ok: boolean
          id: number
        }>
    },
    skills: {
      list: (options?: { includeDisabled?: boolean }) =>
        ipcRenderer.invoke('habit:skills:list', options ?? {}) as Promise<
          Array<{
            id: number
            name: string
            description: string | null
            trigger: string | null
            steps: unknown[]
            source: string | null
            candidateId: number | null
            enabled: boolean
            createdAt: number
            updatedAt: number
            lastUsedAt: number | null
          }>
        >,
      get: (id: number) =>
        ipcRenderer.invoke('habit:skills:get', { id }) as Promise<unknown>,
      create: (input: {
        name: string
        description?: string | null
        trigger?: string | null
        steps: unknown[]
        source?: string
        candidateId?: number | null
        enabled?: boolean
      }) =>
        ipcRenderer.invoke('habit:skills:create', input) as Promise<{ ok: boolean; id: number }>,
      update: (id: number, patch: {
        name?: string
        description?: string | null
        trigger?: string | null
        steps?: unknown[]
        enabled?: boolean
      }) =>
        ipcRenderer.invoke('habit:skills:update', { id, patch }) as Promise<{ ok: boolean }>,
      delete: (id: number) =>
        ipcRenderer.invoke('habit:skills:delete', { id }) as Promise<{ ok: boolean }>,
      touchLastUsed: (id: number) =>
        ipcRenderer.invoke('habit:skills:touch-last-used', { id }) as Promise<{ ok: boolean }>,
      importDir: (sourceDir?: string) =>
        ipcRenderer.invoke('habit:skills:import-dir', { sourceDir }) as Promise<{
          ok: boolean
          canceled?: boolean
          imported: number
          skillIds: number[]
          error?: string
        }>
    },
    localSkills: {
      scan: (options?: { targetRepo?: string | null }) =>
        ipcRenderer.invoke('habit:local-skills:scan', options ?? {}) as Promise<{
          sources: Array<{
            id: string
            name: string
            path: string
            kind: 'default' | 'project' | 'custom'
            skillCount: number
            enabledCount: number
          }>
          skills: Array<{
            id: string
            name: string
            description: string | null
            version: string | null
            dir: string
            skillFile: string
            sourceId: string
            sourceName: string
            sourcePath: string
            enabled: boolean
            health: 'ok' | 'missing-file' | 'invalid'
            frontmatter: Record<string, string>
            markdown: string
            preview: string
            updatedAt: string | null
          }>
          totals: {
            discovered: number
            enabled: number
            disabled: number
          }
          scannedAt: string
        }>,
      addSource: (sourceDir?: string, options?: { targetRepo?: string | null }) =>
        ipcRenderer.invoke('habit:local-skills:add-source', {
          sourceDir,
          targetRepo: options?.targetRepo ?? null
        }) as Promise<{
          ok: boolean
          canceled?: boolean
          snapshot: unknown
        }>,
      setEnabled: (id: string, enabled: boolean, options?: { targetRepo?: string | null }) =>
        ipcRenderer.invoke('habit:local-skills:set-enabled', {
          id,
          enabled,
          targetRepo: options?.targetRepo ?? null
        }) as Promise<{
          ok: boolean
          snapshot: unknown
        }>,
      openPath: (path: string) =>
        ipcRenderer.invoke('habit:local-skills:open-path', { path }) as Promise<{
          ok: boolean
          error?: string
        }>
    },
    skillPipelines: {
      list: (targetRepo: string) =>
        ipcRenderer.invoke('habit:skill-pipelines:list', { targetRepo }) as Promise<
          Array<{
            id: string
            name: string
            description: string | null
            nodeCount: number
            edgeCount: number
            updatedAt: string
          }>
        >,
      read: (targetRepo: string, id: string) =>
        ipcRenderer.invoke('habit:skill-pipelines:read', { targetRepo, id }) as Promise<unknown>,
      save: (targetRepo: string, pipeline: unknown) =>
        ipcRenderer.invoke('habit:skill-pipelines:save', { targetRepo, pipeline }) as Promise<
          | { ok: true; pipeline: unknown }
          | { ok: false; error: string; errors?: string[] }
        >,
      delete: (targetRepo: string, id: string) =>
        ipcRenderer.invoke('habit:skill-pipelines:delete', { targetRepo, id }) as Promise<{
          ok: boolean
        }>
    }
  }
}

contextBridge.exposeInMainWorld('api', api)

export type Api = typeof api
