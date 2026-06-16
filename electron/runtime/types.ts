import type { BuildOutputEncoding, BuildStepEnvType } from '../build/types.js'

export type RuntimeEnvType = BuildStepEnvType
export type RuntimeOutputEncoding = BuildOutputEncoding
export type RuntimeStatus = 'idle' | 'running' | 'exited' | 'failed' | 'stopped'

export interface ProjectRuntimeConfig {
  enabled: boolean
  cwd: string
  command: string
  envType: RuntimeEnvType
  visualStudioInstanceId: string
  outputEncoding: RuntimeOutputEncoding
}

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

export interface StartRuntimeRequest {
  projectId: string
  projectName: string
  targetRepo: string
  config: ProjectRuntimeConfig
}

export type RuntimeStartResult =
  | { ok: true; state: RuntimeState }
  | { ok: false; error: string; state: RuntimeState }

export type RuntimeStopResult = { ok: true } | { ok: false; error: string }

export type RuntimeAnalysisPromptResult =
  | { ok: true; prompt: string }
  | { ok: false; error: string }
