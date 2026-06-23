export function canStartMainSession(
  projectId: string | null,
  noPlanMode: boolean,
  planName: string
): boolean {
  return projectId !== null && (noPlanMode || planName.trim().length > 0)
}

export function formatMainSessionPlanLabel(
  noPlanMode: boolean,
  planName: string
): string {
  if (noPlanMode) return '任务值守模式'
  return planName.trim() || '(未选择方案)'
}
