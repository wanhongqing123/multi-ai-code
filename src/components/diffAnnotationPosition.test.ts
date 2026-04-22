import { describe, expect, it } from 'vitest'
import { placeDraftButton } from './diffAnnotationPosition.js'

describe('placeDraftButton', () => {
  it('keeps the annotation button inside the diff pane when selection is very wide', () => {
    const pos = placeDraftButton({
      paneWidth: 640,
      paneScrollTop: 120,
      paneRectLeft: 100,
      paneRectTop: 40,
      selectionRectRight: 980,
      selectionRectTop: 180
    })

    expect(pos.x).toBeLessThanOrEqual(640 - 96 - 8)
    expect(pos.x).toBeGreaterThanOrEqual(8)
    expect(pos.y).toBe(256)
  })

  it('keeps the annotation button away from the left edge', () => {
    const pos = placeDraftButton({
      paneWidth: 640,
      paneScrollTop: 0,
      paneRectLeft: 100,
      paneRectTop: 40,
      selectionRectRight: 80,
      selectionRectTop: 45
    })

    expect(pos.x).toBe(8)
    expect(pos.y).toBe(1)
  })
})
