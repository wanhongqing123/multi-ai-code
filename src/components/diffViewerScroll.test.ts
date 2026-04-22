import { describe, expect, it } from 'vitest'
import { getHorizontalTrackpadDelta } from './diffViewerScroll.js'

describe('getHorizontalTrackpadDelta', () => {
  it('uses native horizontal deltas from the trackpad gesture', () => {
    expect(
      getHorizontalTrackpadDelta({ deltaX: 48, deltaY: 0, shiftKey: false })
    ).toBe(48)
  })

  it('maps shift+wheel vertical movement to horizontal scrolling', () => {
    expect(
      getHorizontalTrackpadDelta({ deltaX: 0, deltaY: 30, shiftKey: true })
    ).toBe(30)
  })

  it('ignores plain vertical wheel movement', () => {
    expect(
      getHorizontalTrackpadDelta({ deltaX: 0, deltaY: 30, shiftKey: false })
    ).toBe(0)
  })
})
