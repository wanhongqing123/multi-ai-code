export function canSendRepoAnnotations(
  sessionRunning: boolean,
  annotationCount: number,
  sending = false
): boolean {
  return sessionRunning && annotationCount > 0 && !sending
}

export function repoSendButtonTitle(
  sessionRunning: boolean,
  annotationCount: number,
  sending = false
): string {
  if (sending) return '发送中…'
  if (!sessionRunning) return '请先启动下方 AI CLI'
  if (annotationCount <= 0) return '至少需要一条标注'
  return '注入到下方 AI CLI'
}
