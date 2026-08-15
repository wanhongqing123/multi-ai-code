import { contextBridge, ipcRenderer, IpcRendererEvent, webUtils } from 'electron'

// 第二批类型去重：ScheduledTask* 的真源在各自 types.ts（均为纯类型模块）。
import type {
  ScheduledTaskScheduleType,
  ScheduledTaskRunStatus,
  ScheduledTaskRun,
  ScheduledTask,
  ScheduledTaskImageAttachment,
  ReadScheduledTaskImageInput,
  ReadScheduledTaskImageResult,
  SaveScheduledTaskImageInput,
  SaveScheduledTaskImageResult,
  CreateScheduledTaskInput,
  UpdateScheduledTaskInput
} from './scheduledTasks/types.js'

export type {
  ScheduledTaskScheduleType,
  ScheduledTaskRunStatus,
  ScheduledTaskRun,
  ScheduledTask,
  ScheduledTaskImageAttachment,
  ReadScheduledTaskImageInput,
  ReadScheduledTaskImageResult,
  SaveScheduledTaskImageInput,
  SaveScheduledTaskImageResult,
  CreateScheduledTaskInput,
  UpdateScheduledTaskInput
}

// 以下类型的「真源」在各自模块（均为纯类型模块，无运行时/Node 依赖）。这里 import 供
// preload 自身使用，并统一 re-export——渲染层继续从 preload 引类型即可，无需改动。
import type { AiPermissionMode, AiSettings } from './settings/types.js'
import type {
  RemoteDesktopCaptureSource,
  RemoteDesktopEnterRoomParams,
  RemoteDesktopEngine
} from './remote-desktop/engine.js'
import { createTrtcRemoteDesktopEngine } from './remote-desktop/preloadEngine.js'
import type { OpenCodeProviderProfile } from './aicli/opencodeConfig.js'
import type {
  RemoteImContactRelation,
  RemoteImConfig,
  RemoteImAccountConfig,
  RemoteImLoginState,
  RemoteImConnectionState,
  RemoteImStatus,
  RemoteImMessageRole,
  RemoteImMessageDirection,
  RemoteImMessageKind,
  RemoteImMessageOrigin,
  RemoteImMessageStatus,
  RemoteImImageAttachment,
  RemoteImFileAttachment,
  RemoteImMessageAttachment,
  RemoteImMessage,
  ReadRemoteImImagePreviewInput,
  ReadRemoteImImagePreviewResult,
  RemoteImIncomingTextMessage,
  RemoteImRoamedTextMessage,
  RemoteImIncomingAudioMessage,
  RemoteImIncomingImageMessage,
  RemoteImIncomingFileMessage,
  RemoteImRuntimeIdentity,
  RemoteImRuntimeLogEntryInput
} from './remote-im/types.js'

export type {
  AiPermissionMode,
  AiSettings,
  OpenCodeProviderProfile,
  RemoteImContactRelation,
  RemoteImConfig,
  RemoteImAccountConfig,
  RemoteImLoginState,
  RemoteImConnectionState,
  RemoteImStatus,
  RemoteImMessageRole,
  RemoteImMessageDirection,
  RemoteImMessageKind,
  RemoteImMessageOrigin,
  RemoteImMessageStatus,
  RemoteImImageAttachment,
  RemoteImFileAttachment,
  RemoteImMessageAttachment,
  RemoteImMessage,
  ReadRemoteImImagePreviewInput,
  ReadRemoteImImagePreviewResult,
  RemoteImIncomingTextMessage,
  RemoteImRoamedTextMessage,
  RemoteImIncomingAudioMessage,
  RemoteImIncomingImageMessage,
  RemoteImIncomingFileMessage,
  RemoteImRuntimeIdentity,
  RemoteImRuntimeLogEntryInput
}

export interface RemoteImSendPeerImageInput {
  fileToken: string
  toUserId?: string | null
  fileName?: string | null
  mimeType?: string | null
  sizeBytes?: number | null
}

export interface RemoteImSendPeerFileInput {
  localPath: string
  toUserId?: string | null
}

export interface RemoteImOutgoingImageEvent {
  projectId: string
  toUserId: string
  origin: RemoteImMessageOrigin
  runtimeIdentity: RemoteImRuntimeIdentity
  fileToken?: string | null
  fileName?: string | null
  mimeType?: string | null
  fileBytes?: Uint8Array | ArrayBuffer | number[] | null
  messageId?: number | null
}

export interface RemoteImOutgoingFileEvent {
  projectId: string
  toUserId: string
  origin: RemoteImMessageOrigin
  runtimeIdentity: RemoteImRuntimeIdentity
  fileName?: string | null
  mimeType?: string | null
  fileBytes?: Uint8Array | ArrayBuffer | number[] | null
  messageId?: number | null
}

export interface ProjectAiSettingsResponse {
  ok: boolean
  value?: AiSettings
  repaired?: boolean
  error?: string
}

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
  /** CLI binary (claude | codex). */
  command: string
  /** CLI args. */
  args: string[]
  env?: Record<string, string>
  opencode?: OpenCodeProviderProfile
  /** 宿主终端当前明暗主题，用于给 codex 注入正确的默认背景/前景色。 */
  terminalTheme?: 'light' | 'dark'
  cols?: number
  rows?: number
  /**
   * 'new' (default) spawns a fresh CLI session.
   * 'resume' rewrites args to the CLI's native continue form (claude
   * --continue / codex resume --last) so the CLI's own saved conversation
   * history is picked up instead of starting over.
   */
  mode?: 'new' | 'resume'
}

export interface ResolveLaunchRequest {
  command: string
  env?: Record<string, string>
}

export interface ResolveLaunchResponse {
  ok: boolean
  notice?: string
  error?: string
}

export interface SpawnResponse {
  ok: boolean
  error?: string
  launchNotice?: string
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

let remoteDesktopEngine: RemoteDesktopEngine | null = null

function getRemoteDesktopEngine(): RemoteDesktopEngine {
  remoteDesktopEngine ??= createTrtcRemoteDesktopEngine()
  return remoteDesktopEngine
}

const api = {
  platform: process.platform,
  /** 无边框窗口的页面内自绘窗口按钮（Windows；macOS 用系统红绿灯）。 */
  windowControls: {
    minimize: () => ipcRenderer.send('window-controls:minimize'),
    toggleMaximize: () => ipcRenderer.send('window-controls:toggle-maximize'),
    close: () => ipcRenderer.send('window-controls:close')
  },
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
    /** Open an HTTP/HTTPS URL with the operating system's default browser. */
    openExternal: (url: string) =>
      ipcRenderer.invoke('shell:open-external', { url }) as Promise<{
        ok: boolean
        error?: string
      }>,
    /** Open an MSYS shell window with cwd set to the given dir. */
    openMsysTerminal: (cwd: string) =>
      ipcRenderer.invoke('shell:open-msys-terminal', { cwd }) as Promise<{
        ok: boolean
        variant?: 'msys2' | 'git' | 'path' | null
        error?: string
      }>
  },
  // 远程桌面被控端引擎。真实现必须住在 preload：主进程 require 会因模块级
  // DOM 访问而失败，渲染进程又是 nodeIntegration:false 拿不到 require。
  // 引擎是惰性创建的，没开启远程桌面的用户不会加载那 29MB 原生二进制。
  remoteDesktop: {
    listScreenSources: () => getRemoteDesktopEngine().listScreenSources(),
    startSharing: (params: RemoteDesktopEnterRoomParams, source: RemoteDesktopCaptureSource) =>
      getRemoteDesktopEngine().startSharing(params, source),
    stopSharing: () => getRemoteDesktopEngine().stopSharing()
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
    // 登录门专用：绑定账号 = 用它初始化账号作用域数据层 + 抢每账号单实例锁 + 写账号配置。
    // alreadyLocked 表示该账号已在另一个窗口打开。成功后 value 是登录态，渲染层据此放行主界面。
    bindAccount: (account: RemoteImAccountConfig) =>
      ipcRenderer.invoke('remote-im:bind-account', { account }) as Promise<
        | { ok: true; value: RemoteImLoginState }
        | { ok: false; error: string; alreadyLocked?: boolean }
      >,
    setConfig: (projectId: string, config: RemoteImConfig) =>
      ipcRenderer.invoke('remote-im:set-config', { projectId, config }) as Promise<
        | { ok: true; value: RemoteImConfig; repaired?: true }
        | { ok: false; error: string; details?: Array<{ path: string; message: string }> }
      >,
    getStatus: (projectId: string) =>
      ipcRenderer.invoke('remote-im:get-status', { projectId }) as Promise<RemoteImStatus>,
    listMessages: (projectId: string, limit = 500) =>
      ipcRenderer.invoke('remote-im:list-messages', { projectId, limit }) as Promise<
        RemoteImMessage[]
      >,
    // 消息汇总视图：取回项目最近的消息全集（默认 3000 条，服务端硬顶 5000）。
    listMessagesForSummary: (projectId: string, limit = 3000) =>
      ipcRenderer.invoke('remote-im:list-messages-for-summary', { projectId, limit }) as Promise<
        RemoteImMessage[]
      >,
    readImagePreview: (input: ReadRemoteImImagePreviewInput) =>
      ipcRenderer.invoke('remote-im:read-image-preview', input) as Promise<
        ReadRemoteImImagePreviewResult
      >,
    // 消息汇总落盘为 .md 文件，返回绝对路径（供发送给 AICLI 读取）。
    saveSummaryMarkdown: (projectId: string, markdown: string) =>
      ipcRenderer.invoke('remote-im:save-summary-markdown', { projectId, markdown }) as Promise<
        { ok: true; path: string } | { ok: false; error: string }
      >,
    listPeerMessagesBefore: (
      projectId: string,
      peerUserId: string,
      before: { createdAt: number; id: number },
      limit = 200
    ) =>
      ipcRenderer.invoke('remote-im:list-peer-messages-before', {
        projectId,
        peerUserId,
        beforeCreatedAt: before.createdAt,
        beforeId: before.id,
        limit
      }) as Promise<RemoteImMessage[]>,
    deleteContact: (projectId: string, userId: string) =>
      ipcRenderer.invoke('remote-im:delete-contact', { projectId, userId }) as Promise<
        | { ok: true; value: RemoteImConfig; loginState: RemoteImLoginState }
        | { ok: false; error: string }
      >,
    syncContacts: (
      projectId: string,
      userIds: string[],
      runtimeIdentity: RemoteImRuntimeIdentity
    ) =>
      ipcRenderer.invoke('remote-im:sync-contacts', {
        projectId,
        userIds,
        runtimeIdentity
      }) as Promise<
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
    sendPeerImage: (projectId: string, file: File, image: RemoteImSendPeerImageInput) =>
      ipcRenderer.invoke('remote-im:send-peer-image', {
        projectId,
        ...image,
        localPath: webUtils.getPathForFile(file) || null
      }) as Promise<
        { ok: boolean; error?: string; toUserId?: string }
      >,
    sendPeerFile: (projectId: string, file: RemoteImSendPeerFileInput) =>
      ipcRenderer.invoke('remote-im:send-peer-file', { projectId, ...file }) as Promise<
        { ok: boolean; error?: string; toUserId?: string }
      >,
    readFilePreview: (input: { localPath?: string | null; mimeType?: string | null }) =>
      ipcRenderer.invoke('remote-im:read-file-preview', input) as Promise<
        | { ok: true; value: { content: string; mimeType: string; fileName: string } }
        | { ok: false; error: string }
      >,
    registerRuntime: (projectId: string, runtimeIdentity: RemoteImRuntimeIdentity) =>
      ipcRenderer.invoke('remote-im:register-runtime', { projectId, runtimeIdentity }) as Promise<{
        ok: boolean
        error?: string
      }>,
    deliverIncomingText: (
      message: RemoteImIncomingTextMessage,
      runtimeIdentity: RemoteImRuntimeIdentity
    ) =>
      ipcRenderer.invoke('remote-im:deliver-incoming-text', {
        message,
        runtimeIdentity
      }) as Promise<{
        ok: boolean
        error?: string
      }>,
    deliverIncomingAudio: (
      message: RemoteImIncomingAudioMessage,
      runtimeIdentity: RemoteImRuntimeIdentity
    ) =>
      ipcRenderer.invoke('remote-im:deliver-incoming-audio', {
        message,
        runtimeIdentity
      }) as Promise<{
        ok: boolean
        error?: string
      }>,
    deliverIncomingImage: (
      message: RemoteImIncomingImageMessage,
      runtimeIdentity: RemoteImRuntimeIdentity
    ) =>
      ipcRenderer.invoke('remote-im:deliver-incoming-image', {
        message,
        runtimeIdentity
      }) as Promise<{
        ok: boolean
        error?: string
      }>,
    deliverIncomingFile: (
      message: RemoteImIncomingFileMessage,
      runtimeIdentity: RemoteImRuntimeIdentity
    ) =>
      ipcRenderer.invoke('remote-im:deliver-incoming-file', {
        message,
        runtimeIdentity
      }) as Promise<{
        ok: boolean
        error?: string
      }>,
    backfillRoamedText: (payload: {
      projectId: string
      messages: RemoteImRoamedTextMessage[]
      runtimeIdentity: RemoteImRuntimeIdentity
    }) =>
      ipcRenderer.invoke('remote-im:backfill-roamed-text', payload) as Promise<{
        ok: boolean
        inserted?: number
        error?: string
      }>,
    updateSdkStatus: (status: Pick<RemoteImStatus, 'projectId' | 'state' | 'detail'>) =>
      ipcRenderer.invoke('remote-im:update-sdk-status', { status }) as Promise<{
        ok: boolean
        error?: string
      }>,
    updateSdkRuntimeStatus: (
      status: Pick<RemoteImStatus, 'projectId' | 'state' | 'detail'>,
      runtimeIdentity: RemoteImRuntimeIdentity
    ) =>
      ipcRenderer.invoke('remote-im:update-sdk-status', { status, runtimeIdentity }) as Promise<{
        ok: boolean
        error?: string
      }>,
    markOutgoingMessageSent: (
      projectId: string,
      messageId: number,
      remoteMessageId: string | null | undefined,
      runtimeIdentity: RemoteImRuntimeIdentity
    ) =>
      ipcRenderer.invoke('remote-im:mark-outgoing-message-sent', {
        projectId,
        messageId,
        remoteMessageId: remoteMessageId ?? null,
        runtimeIdentity
      }) as Promise<{ ok: true }>,
    markOutgoingMessageFailed: (
      projectId: string,
      messageId: number,
      error: string,
      runtimeIdentity: RemoteImRuntimeIdentity
    ) =>
      ipcRenderer.invoke('remote-im:mark-outgoing-message-failed', {
        projectId,
        messageId,
        error,
        runtimeIdentity
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
      cb: (evt: {
        projectId: string
        toUserId: string
        text: string
        origin: RemoteImMessageOrigin
        runtimeIdentity: RemoteImRuntimeIdentity
        messageId?: number | null
      }) => void
    ) => {
      const handler = (
        _event: IpcRendererEvent,
        evt: {
          projectId: string
          toUserId: string
          text: string
          origin: RemoteImMessageOrigin
          runtimeIdentity: RemoteImRuntimeIdentity
          messageId?: number | null
        }
      ) => cb(evt)
      ipcRenderer.on('remote-im:outgoing-text', handler)
      return () => ipcRenderer.removeListener('remote-im:outgoing-text', handler)
    },
    onOutgoingImage: (
      cb: (evt: RemoteImOutgoingImageEvent) => void
    ) => {
      const handler = (
        _event: IpcRendererEvent,
        evt: RemoteImOutgoingImageEvent
      ) => cb(evt)
      ipcRenderer.on('remote-im:outgoing-image', handler)
      return () => ipcRenderer.removeListener('remote-im:outgoing-image', handler)
    },
    onOutgoingFile: (
      cb: (evt: RemoteImOutgoingFileEvent) => void
    ) => {
      const handler = (
        _event: IpcRendererEvent,
        evt: RemoteImOutgoingFileEvent
      ) => cb(evt)
      ipcRenderer.on('remote-im:outgoing-file', handler)
      return () => ipcRenderer.removeListener('remote-im:outgoing-file', handler)
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
  launchNewInstance: () => ipcRenderer.send('app:launch-new-instance'),
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

  scheduledTasks: {
    list: (projectId: string) =>
      ipcRenderer.invoke('scheduled-tasks:list', { projectId }) as Promise<
        ScheduledTask[]
      >,
    create: (input: CreateScheduledTaskInput) =>
      ipcRenderer.invoke('scheduled-tasks:create', input) as Promise<
        { ok: true; task: ScheduledTask } | { ok: false; error: string }
      >,
    saveImage: (input: SaveScheduledTaskImageInput) =>
      ipcRenderer.invoke('scheduled-tasks:save-image', input) as Promise<
        SaveScheduledTaskImageResult
      >,
    readImage: (input: ReadScheduledTaskImageInput) =>
      ipcRenderer.invoke('scheduled-tasks:read-image', input) as Promise<
        ReadScheduledTaskImageResult
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
    analysisStart: (req: {
      projectId: string
      targetRepo: string
      command: string
      args: string[]
      env?: Record<string, string>
      opencode?: OpenCodeProviderProfile
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
    resolveLaunch: (opts: ResolveLaunchRequest) =>
      ipcRenderer.invoke('cc:resolve-launch', opts) as Promise<ResolveLaunchResponse>,
    spawn: (opts: SpawnRequest) =>
      ipcRenderer.invoke('cc:spawn', opts) as Promise<SpawnResponse>,
    write: (sessionId: string, data: string) =>
      ipcRenderer.send('cc:input', { sessionId, data }),
    // 广播明暗主题给所有运行中的 AICLI 会话，TUI 运行时重绘、无需重启。
    setTerminalTheme: (theme: 'light' | 'dark') =>
      ipcRenderer.send('cc:set-terminal-theme', { theme }),
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
  }
}

contextBridge.exposeInMainWorld('api', api)

export type Api = typeof api
