export type AiCliKind = 'claude' | 'codex'

export function getCliTargetLabel(aiCli: AiCliKind): string {
  return aiCli === 'codex' ? 'codex cli' : 'claude cli'
}
