export function nextRuntimeLogDialogOpenAfterSendResult(currentOpen: boolean, sendOk: boolean): boolean {
  return sendOk ? false : currentOpen
}
