import { describe, expect, it } from 'vitest'
import {
  createInitialRuntimeLogFrame,
  moveRuntimeLogFrame,
  resizeRuntimeLogFrame,
  type RuntimeLogFloatingFrame
} from '../../../src/utils/runtimeLogFloatingFrame.js'

const bounds = { width: 1440, height: 900 }
const frame: RuntimeLogFloatingFrame = {
  left: 700,
  top: 100,
  width: 600,
  height: 500
}

describe('runtimeLogFloatingFrame', () => {
  it('places the initial frame as a right-side floating panel', () => {
    expect(createInitialRuntimeLogFrame(bounds)).toEqual({
      left: 700,
      top: 84,
      width: 720,
      height: 796
    })
  })

  it('moves the frame and keeps it inside the viewport', () => {
    expect(moveRuntimeLogFrame(frame, -200, 50, bounds)).toMatchObject({
      left: 500,
      top: 150,
      width: 600,
      height: 500
    })
    expect(moveRuntimeLogFrame(frame, -2000, -2000, bounds)).toMatchObject({
      left: 16,
      top: 16
    })
  })

  it('resizes from the left edge by changing left and width together', () => {
    expect(resizeRuntimeLogFrame(frame, 'w', -120, 0, bounds)).toMatchObject({
      left: 580,
      width: 720,
      top: 100,
      height: 500
    })
  })

  it('resizes from the top edge by changing top and height together', () => {
    expect(resizeRuntimeLogFrame(frame, 'n', 0, -80, bounds)).toMatchObject({
      left: 700,
      width: 600,
      top: 20,
      height: 580
    })
  })

  it('resizes from a corner across both axes', () => {
    expect(resizeRuntimeLogFrame(frame, 'se', 100, 120, bounds)).toMatchObject({
      left: 700,
      top: 100,
      width: 700,
      height: 620
    })
  })
})
