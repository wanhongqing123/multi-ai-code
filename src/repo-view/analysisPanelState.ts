export function canSendRepoAnnotations(
  sessionRunning: boolean,
  annotationCount: number
): boolean {
  return sessionRunning && annotationCount > 0
}

export function repoSendButtonTitle(
  sessionRunning: boolean,
  annotationCount: number
): string {
  if (!sessionRunning) return '请先启动下方 AI CLI'
  if (annotationCount <= 0) return '至少需要一条标注'
  return '注入到下方 AI CLI'
}
