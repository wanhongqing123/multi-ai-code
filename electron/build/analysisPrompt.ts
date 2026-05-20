import type { BuildFailureContext, BuildRuntimeState } from './types.js'

function renderStepSummary(state: BuildRuntimeState): string[] {
  return state.steps.map((step) => `- ${step.name} [${step.id}]: ${step.status}`)
}

export function buildFailureAnalysisPrompt(state: BuildRuntimeState): string {
  const failure = state.lastFailure
  if (!failure) {
    throw new Error('no failed build context available')
  }

  return [
    'Analyze only the failure cause for this failed build run.',
    'Do not modify code.',
    'Do not provide patches.',
    'Do not execute commands.',
    'Do not suggest command execution.',
    '',
    `Project: ${failure.projectName ?? 'Unknown'} (${failure.projectId})`,
    `Target repo: ${failure.targetRepo}`,
    `Step: ${failure.stepName} (${failure.stepId})`,
    `Environment: ${failure.envType}`,
    `Working directory: ${failure.cwd}`,
    `Command: ${failure.command}`,
    `Exit code: ${failure.exitCode ?? 'n/a'}`,
    `Signal: ${failure.signal ?? 'n/a'}`,
    `Failure summary: ${failure.reason}`,
    '',
    'Step summary:',
    ...renderStepSummary(state),
    '',
    'Relevant log tail:',
    failure.logTail || '(no log tail captured)',
    '',
    'Reply using exactly these sections:',
    'Failure category',
    'Most likely cause',
    'Evidence',
    'What to check first',
  ].join('\n')
}

export function getFailureAnalysisPrompt(
  state: BuildRuntimeState
): { ok: true; prompt: string } | { ok: false; error: string } {
  if (state.status !== 'failed' || !state.lastFailure) {
    return { ok: false, error: 'no failed build context available' }
  }
  return { ok: true, prompt: buildFailureAnalysisPrompt(state) }
}
