export type AiCliKind = 'claude' | 'codex' | 'opencode' | 'claw'

// Record 而不是 if 链：加第五种 AICLI 时漏掉这里会是**编译错误**，而不是静默回退到
// 'claude cli'。原来的 if 链对遗漏是无声的。
const CLI_TARGET_LABELS: Record<AiCliKind, string> = {
  claude: 'claude cli',
  codex: 'codex cli',
  opencode: 'opencode cli',
  claw: 'claw cli'
}

export function getCliTargetLabel(aiCli: AiCliKind): string {
  return CLI_TARGET_LABELS[aiCli] ?? CLI_TARGET_LABELS.claude
}
