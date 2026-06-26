export type BuildStepEnvType = 'system' | 'msys' | 'visual-studio'
export type BuildOutputEncoding = 'auto' | 'utf8' | 'gbk'
export type BuildExecutionScope = 'all' | 'single-step'

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

export type BuildStepStatus =
  | 'not-run'
  | 'pending'
  | 'running'
  | 'succeeded'
  | 'failed'
  | 'skipped'
export type BuildOverallStatus = 'idle' | 'running' | 'succeeded' | 'failed' | 'stopped'

export interface BuildStepRuntime extends BuildStepConfig {
  visualStudioDisplayName: string | null
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

export interface BuildRuntimeState {
  status: BuildOverallStatus
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

export interface StartBuildRequest {
  projectId: string
  projectName: string
  targetRepo: string
  config: ProjectBuildConfig
  scope?: BuildExecutionScope
  stepId?: string | null
}

export type BuildStartResult =
  | { ok: true; state: BuildRuntimeState }
  | { ok: false; error: string; state: BuildRuntimeState }

export type BuildStopResult = { ok: true } | { ok: false; error: string }

export type BuildFailureAnalysisPromptResult =
  | { ok: true; prompt: string }
  | { ok: false; error: string }
