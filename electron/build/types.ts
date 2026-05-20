export type BuildStepEnvType = 'msys' | 'visual-studio'

export interface BuildStepConfig {
  id: string
  name: string
  envType: BuildStepEnvType
  cwd: string
  command: string
  enabled: boolean
}

export interface ProjectBuildConfig {
  enabled: boolean
  steps: BuildStepConfig[]
}

export type BuildStepStatus = 'pending' | 'running' | 'succeeded' | 'failed' | 'skipped'
export type BuildOverallStatus = 'idle' | 'running' | 'succeeded' | 'failed' | 'stopped'

export interface BuildStepRuntime extends BuildStepConfig {
  status: BuildStepStatus
  resolvedCwd: string | null
  startedAt: string | null
  finishedAt: string | null
  exitCode: number | null
  signal: NodeJS.Signals | null
}

export interface BuildFailureContext {
  projectId: string
  projectName: string | null
  targetRepo: string
  stepId: string
  stepName: string
  envType: BuildStepEnvType
  cwd: string
  command: string
  exitCode: number | null
  signal: NodeJS.Signals | null
  reason: string
  logTail: string
}

export interface BuildRuntimeState {
  status: BuildOverallStatus
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

export interface StartBuildRequest {
  projectId: string
  projectName: string
  targetRepo: string
  config: ProjectBuildConfig
}

export type BuildStartResult =
  | { ok: true; state: BuildRuntimeState }
  | { ok: false; error: string; state: BuildRuntimeState }

export type BuildStopResult = { ok: true } | { ok: false; error: string }

export type BuildFailureAnalysisPromptResult =
  | { ok: true; prompt: string }
  | { ok: false; error: string }
