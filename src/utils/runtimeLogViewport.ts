export interface RuntimeLogViewport {
  scrollTop: number
  readonly scrollHeight: number
}

export function scrollRuntimeLogToBottom(element: RuntimeLogViewport | null): void {
  if (!element) return
  element.scrollTop = element.scrollHeight
}
