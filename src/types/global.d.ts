import type {
  SpawnOptions,
  DataEvent,
  ExitEvent,
  StageDoneEvent,
  FeedbackEmittedEvent,
  HandoffInjection,
  FeedbackInjection,
  ArtifactRecord
} from '../../electron/preload'

declare global {
  interface Window {
    api: {
      ping: () => Promise<string>
      version: () => Promise<string>
      demoProject: () => Promise<{ id: string; dir: string; target_repo: string }>
      project: {
        pickDir: () => Promise<{ canceled: boolean; path?: string }>
        setTargetRepo: (
          path: string
        ) => Promise<{
          ok: boolean
          target_repo?: string
          name?: string
          error?: string
        }>
      }
      cc: {
        spawn: (opts: SpawnOptions) => Promise<{ ok: boolean; error?: string }>
        write: (sessionId: string, data: string) => void
        resize: (sessionId: string, cols: number, rows: number) => void
        kill: (sessionId: string) => Promise<{ ok: boolean; error?: string }>
        killAll: () => Promise<{ ok: boolean; killed: string[] }>
        list: () => Promise<string[]>
        has: (sessionId: string) => Promise<boolean>
        onData: (cb: (evt: DataEvent) => void) => () => void
        onExit: (cb: (evt: ExitEvent) => void) => () => void
      }
      stage: {
        onDone: (cb: (evt: StageDoneEvent) => void) => () => void
        onFeedbackEmitted: (cb: (evt: FeedbackEmittedEvent) => void) => () => void
        injectHandoff: (h: HandoffInjection) => Promise<{ ok: boolean; error?: string }>
        injectFeedback: (h: FeedbackInjection) => Promise<{ ok: boolean; error?: string }>
        triggerDone: (req: {
          sessionId: string
          projectId: string
          stageId: number
          projectDir: string
          artifactPath?: string
          verdict?: string
          summary?: string
        }) => Promise<{
          ok: boolean
          artifactFound?: boolean
          snapshotPath?: string | null
          error?: string
        }>
      }
      artifact: {
        list: (projectId: string, stageId?: number) => Promise<ArtifactRecord[]>
        read: (
          projectDir: string,
          path: string
        ) => Promise<{ ok: boolean; content?: string; error?: string }>
      }
    }
  }
}

export {}
