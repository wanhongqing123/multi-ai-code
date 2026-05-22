import type { BuildExecutionScope, ProjectBuildConfig } from './types.js'

export interface BuildExecutionSelection {
  ok: true
  scope: BuildExecutionScope
  requestedStepId: string | null
  runnableStepIds: string[]
}

export interface BuildExecutionSelectionError {
  ok: false
  error: string
}

export function resolveBuildExecutionScope(
  config: ProjectBuildConfig,
  options?: { scope?: BuildExecutionScope; stepId?: string | null }
): BuildExecutionSelection | BuildExecutionSelectionError {
  const scope = options?.scope ?? 'all'

  if (scope === 'all') {
    const runnableStepIds = config.steps.filter((step) => step.enabled).map((step) => step.id)
    return {
      ok: true,
      scope,
      requestedStepId: null,
      runnableStepIds,
    }
  }

  const stepId = typeof options?.stepId === 'string' ? options.stepId.trim() : ''
  if (!stepId) {
    return { ok: false, error: 'build step is required for single-step scope' }
  }

  const target = config.steps.find((step) => step.id === stepId)
  if (!target) {
    return { ok: false, error: `build step not found: ${stepId}` }
  }
  if (!target.enabled) {
    return { ok: false, error: `build step is disabled: ${stepId}` }
  }

  return {
    ok: true,
    scope,
    requestedStepId: stepId,
    runnableStepIds: [stepId],
  }
}
