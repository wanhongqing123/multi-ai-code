export function canStartMainSession(projectId: string | null): boolean {
  return projectId !== null
}

// 标题栏显示当前选中的普通任务。普通任务与定时任务并存，没选普通任务不代表
// 处在别的模式里——定时任务在后台照常调度，与这里显示什么无关。
export function formatMainSessionPlanLabel(planName: string): string {
  return planName.trim() || '(未选择普通任务)'
}
