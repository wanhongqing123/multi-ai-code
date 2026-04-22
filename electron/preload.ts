import { contextBridge, ipcRenderer, IpcRendererEvent, webUtils } from 'electron'

export interface AiSettings {
  ai_cli: 'claude' | 'codex'
  command?: string
  args?: string[]
  env?: Record<string, string>
}

export interface SpawnRequest {
  sessionId: string
  projectId: string
  projectDir: string
  targetRepo: string
  planName: string
  /** Absolute path resolved via resolvePlanArtifactAbs. */
  planAbsPath: string
  /** true when plan file does not yet exist on disk. */
  planPending: boolean
  /** First user message to feed after kickoff. */
  initialUserMessage: string
  /** CLI binary (claude | codex). */
  command: string
  /** CLI args. */
  args: string[]
  env?: Record<string, string>
  cols?: number
  rows?: number
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
  events: {
    list: (projectId: string, limit?: number) =>
      ipcRenderer.invoke('event:list', { projectId, limit }) as Promise<
        Array<{
          id: number
          project_id: string
          from_stage: number | null
          to_stage: number | null
          kind: string
          payload: string | null
          created_at: string
        }>
      >
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
      ipcRenderer.invoke('project:set-stage-configs', { id, configs }) as Promise<{
        ok: boolean
      }>,
    getAiSettings: (id: string) =>
      ipcRenderer.invoke('project:get-ai-settings', { id }) as Promise<AiSettings>,
    setAiSettings: (id: string, settings: AiSettings) =>
      ipcRenderer.invoke('project:set-ai-settings', {
        id,
        settings
      }) as Promise<{ ok: boolean; error?: string }>,
    getRepoViewAiSettings: (id: string) =>
      ipcRenderer.invoke('project:get-repo-view-ai-settings', { id }) as Promise<AiSettings>,
    setRepoViewAiSettings: (id: string, settings: AiSettings) =>
      ipcRenderer.invoke('project:set-repo-view-ai-settings', {
        id,
        settings
      }) as Promise<{ ok: boolean; error?: string }>,
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
    analysisSend: (req: {
      repoRoot: string
      filePath: string
      selection: string
      question: string
      projectSummary: string
      fileNote: string
    }) =>
      ipcRenderer.invoke('repo-view:analysis-send', req) as Promise<{
        ok: boolean
        error?: string
      }>,
    analysisStop: () =>
      ipcRenderer.invoke('repo-view:analysis-stop') as Promise<{
        ok: boolean
        error?: string
      }>,
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
    resize: (sessionId: string, cols: number, rows: number) =>
      ipcRenderer.send('cc:resize', { sessionId, cols, rows }),
    kill: (sessionId: string) =>
      ipcRenderer.invoke('cc:kill', { sessionId }) as Promise<{ ok: boolean; error?: string }>,
    sendUser: (sessionId: string, text: string) =>
      ipcRenderer.invoke('cc:send-user', { sessionId, text }) as Promise<{
        ok: boolean
        error?: string
      }>,
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
        items: { name: string; abs: string; source: 'internal' | 'external' }[]
        error?: string
      }>,
    registerExternal: (req: { projectDir: string; externalPath: string }) =>
      ipcRenderer.invoke('plan:registerExternal', req) as Promise<
        | { ok: true; name: string }
        | { ok: false; error: string }
      >,
    removeExternal: (req: { projectDir: string; name: string }) =>
      ipcRenderer.invoke('plan:removeExternal', req) as Promise<
        { ok: true } | { ok: false; error: string }
      >
  }
}

contextBridge.exposeInMainWorld('api', api)

export type Api = typeof api
