import type { RuntimeAnalysisPromptResult, RuntimeState } from './types.js'

export const RUNTIME_ANALYSIS_LOG_TAIL_LIMIT = 200_000

function tail(value: string, limit: number): string {
  if (value.length <= limit) return value
  return `[runtime] earlier log truncated...\n${value.slice(-limit)}`
}

export function buildRuntimeAnalysisPrompt(
  state: RuntimeState,
  opts: { logTailLimit?: number } = {}
): string {
  const logTail = tail(state.log.trim(), opts.logTailLimit ?? RUNTIME_ANALYSIS_LOG_TAIL_LIMIT)

  return [
    'Analyze this runtime log for likely problems.',
    'Do not modify code.',
    'Do not provide patches.',
    'Do not execute commands.',
    '',
    `Project: ${state.projectName ?? 'Unknown'} (${state.projectId ?? 'unknown'})`,
    `Target repo: ${state.targetRepo ?? 'unknown'}`,
    `Runtime status: ${state.status}`,
    `Environment: ${state.envType ?? 'unknown'}`,
    `Working directory: ${state.cwd ?? 'unknown'}`,
    `Command: ${state.command ?? 'unknown'}`,
    `Exit code: ${state.exitCode ?? 'n/a'}`,
    `Signal: ${state.signal ?? 'n/a'}`,
    '',
    'Recent runtime log:',
    logTail,
    '',
    'Reply using these sections:',
    'Problem summary',
    'Evidence',
    'Likely cause',
    'What to check first',
  ].join('\n')
}

export function getRuntimeAnalysisPrompt(state: RuntimeState): RuntimeAnalysisPromptResult {
  if (!state.log.trim()) {
    return { ok: false, error: 'no runtime log available' }
  }

  return { ok: true, prompt: buildRuntimeAnalysisPrompt(state) }
}
