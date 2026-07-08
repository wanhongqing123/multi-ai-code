export type AiCliKind = 'claude' | 'codex' | 'opencode'

export function getCliTargetLabel(aiCli: AiCliKind): string {
  if (aiCli === 'codex') return 'codex cli'
  if (aiCli === 'opencode') return 'opencode cli'
  return 'claude cli'
}
