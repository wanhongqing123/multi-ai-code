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
  if (sending) return '发送中...'
  if (!sessionRunning) return '请先启动下方 AI CLI'
  if (annotationCount <= 0) return '至少需要一条标注'
  return '发送'
}

export async function dispatchRepoSendQuestion(
  question: string,
  sendQuestion: (question: string) => Promise<boolean>
): Promise<boolean> {
  return sendQuestion(question.trim())
}
