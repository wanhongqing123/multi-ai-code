export function nextRuntimeLogDialogOpenAfterSendResult(currentOpen: boolean, sendOk: boolean): boolean {
  return sendOk ? false : currentOpen
}

export function nextRuntimeLogCommentAfterSendResult(currentComment: string, sendOk: boolean): string {
  return sendOk ? '' : currentComment
}
