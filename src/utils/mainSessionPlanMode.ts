export function canStartMainSession(
  projectId: string | null,
  _noPlanMode: boolean,
  _planName: string
): boolean {
  return projectId !== null
}

export function formatMainSessionPlanLabel(
  noPlanMode: boolean,
  planName: string
): string {
  if (noPlanMode) return '定时任务'
  return planName.trim() || '(未选择普通任务)'
}
