import { contextBridge, ipcRenderer, IpcRendererEvent, webUtils } from 'electron'

export interface SpawnOptions {
  sessionId: string
  projectId: string
  stageId: number
  projectDir: string
  cwd: string
  command?: string
  args?: string[]
  cols?: number
  rows?: number
  env?: Record<string, string>
  skipSystemPrompt?: boolean
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

export interface FeedbackEmittedEvent {
  sessionId: string
  projectId: string
  fromStage: number
  toStage: number
  raw: string
  params: Record<string, string>
}

export interface HandoffInjection {
  sessionId: string
  fromStage: number
  toStage: number
  artifactPath: string | null
  artifactContent: string | null
  summary?: string
  verdict?: string
  /** Optional — main process auto-loads design/acceptance from this dir. */
  projectDir?: string
}

export interface FeedbackInjection {
  sessionId: string
  fromStage: number
  toStage: number
  note: string
  artifactPath?: string
  artifactContent?: string
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
  ping: () => ipcRenderer.invoke('app:ping'),
  version: () => ipcRenderer.invoke('app:version'),
  demoProject: () =>
    ipcRenderer.invoke('app:demo-project') as Promise<{
      id: string
      dir: string
      target_repo: string
    }>,
  project: {
    pickDir: () =>
      ipcRenderer.invoke('project:pick-dir') as Promise<{
        canceled: boolean
        path?: string
      }>,
    setTargetRepo: (path: string) =>
      ipcRenderer.invoke('project:set-target-repo', { path }) as Promise<{
        ok: boolean
        target_repo?: string
        name?: string
        error?: string
      }>
  },

  cc: {
    spawn: (opts: SpawnOptions) =>
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
    }
  },

  stage: {
    onDone: (cb: (evt: StageDoneEvent) => void): (() => void) => {
      const handler = (_e: IpcRendererEvent, evt: StageDoneEvent) => cb(evt)
      ipcRenderer.on('stage:done', handler)
      return () => ipcRenderer.removeListener('stage:done', handler)
    },
    onFeedbackEmitted: (cb: (evt: FeedbackEmittedEvent) => void): (() => void) => {
      const handler = (_e: IpcRendererEvent, evt: FeedbackEmittedEvent) => cb(evt)
      ipcRenderer.on('stage:feedback-emitted', handler)
      return () => ipcRenderer.removeListener('stage:feedback-emitted', handler)
    },
    injectHandoff: (h: HandoffInjection) =>
      ipcRenderer.invoke('stage:inject-handoff', h) as Promise<{
        ok: boolean
        error?: string
      }>,
    injectFeedback: (h: FeedbackInjection) =>
      ipcRenderer.invoke('stage:inject-feedback', h) as Promise<{
        ok: boolean
        error?: string
      }>,
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
    restore: (req: {
      projectId: string
      projectDir: string
      stageId: number
      snapshotPath: string
      sessionId?: string
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
    }) =>
      ipcRenderer.invoke('artifact:import-file', req) as Promise<{
        ok: boolean
        canceled?: boolean
        artifactPath?: string
        snapshotPath?: string | null
        error?: string
      }>
  }
}

contextBridge.exposeInMainWorld('api', api)

export type Api = typeof api
