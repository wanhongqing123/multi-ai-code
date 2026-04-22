export interface DraftButtonPlacementInput {
  paneWidth: number
  paneScrollTop: number
  paneRectLeft: number
  paneRectTop: number
  selectionRectRight: number
  selectionRectTop: number
}

const DRAFT_BUTTON_W = 96
const DRAFT_BUTTON_PAD = 8

export function placeDraftButton(
  input: DraftButtonPlacementInput
): { x: number; y: number } {
  const rawX = input.selectionRectRight - input.paneRectLeft + 4
  const maxX = Math.max(DRAFT_BUTTON_PAD, input.paneWidth - DRAFT_BUTTON_W - DRAFT_BUTTON_PAD)
  return {
    x: Math.max(DRAFT_BUTTON_PAD, Math.min(rawX, maxX)),
    y: input.selectionRectTop - input.paneRectTop + input.paneScrollTop - 4
  }
}
