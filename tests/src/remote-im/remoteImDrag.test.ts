import { describe, expect, it } from 'vitest'
import {
  clampRemoteImPanelPosition,
  getDraggedRemoteImPanelPosition,
  getInitialRemoteImPanelPosition
} from '../../../src/remote-im/remoteImDrag.js'

describe('remote IM panel dragging', () => {
  const frame = {
    viewportWidth: 1440,
    viewportHeight: 900,
    panelWidth: 560,
    panelHeight: 720
  }

  it('keeps the default panel position near the current top-right placement', () => {
    expect(getInitialRemoteImPanelPosition(frame)).toEqual({ x: 844, y: 92 })
  })

  it('clamps dragged panel position inside the viewport', () => {
    expect(clampRemoteImPanelPosition({ x: -100, y: -40 }, frame)).toEqual({ x: 16, y: 16 })
    expect(clampRemoteImPanelPosition({ x: 2000, y: 1200 }, frame)).toEqual({
      x: 864,
      y: 164
    })
  })

  it('applies pointer delta from drag start before clamping', () => {
    expect(
      getDraggedRemoteImPanelPosition({
        startPosition: { x: 844, y: 92 },
        startPointer: { x: 100, y: 100 },
        currentPointer: { x: 40, y: 160 },
        frame
      })
    ).toEqual({ x: 784, y: 152 })
  })
})
