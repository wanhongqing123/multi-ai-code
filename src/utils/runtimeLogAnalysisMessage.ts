export function buildRuntimeLogAnalysisMessage(baseMessage: string, comment: string): string {
  const trimmedComment = comment.trim()
  if (!trimmedComment) return baseMessage
  return `${baseMessage}\n\nUser question/comment:\n${trimmedComment}`
}
