import { describe, expect, it } from 'vitest'
import { toAbsoluteCoordinates } from '../../../electron/remote-desktop/inputSinkWindows.js'

const singleScreen = { left: 0, top: 0, width: 2560, height: 1600 }

describe('normalized coordinates to SendInput absolute coordinates', () => {
  it('maps the corners of a single-screen desktop to the full 0..65535 range', () => {
    expect(toAbsoluteCoordinates(0, 0, singleScreen, singleScreen)).toEqual({ absX: 0, absY: 0 })
    expect(toAbsoluteCoordinates(1, 1, singleScreen, singleScreen)).toEqual({
      absX: 65535,
      absY: 65535
    })
  })

  it('puts the centre in the middle', () => {
    // 分母是 width-1（65535 对应最后一个像素而非像素数），所以中点必然偏
    // 半个像素。1600 高时约 41 单位/像素，容差按"不超过一个像素"给。
    const onePixelX = 65535 / singleScreen.width
    const onePixelY = 65535 / singleScreen.height
    const { absX, absY } = toAbsoluteCoordinates(0.5, 0.5, singleScreen, singleScreen)
    expect(Math.abs(absX - 32768)).toBeLessThanOrEqual(onePixelX)
    expect(Math.abs(absY - 32768)).toBeLessThanOrEqual(onePixelY)
  })

  it('keeps a secondary screen off the primary one', () => {
    // 共享右侧副屏时，归一化 (0,0) 是副屏左上角，不是桌面原点。
    // 少了这一步换算，远程点击会全落在主屏上。
    const virtual = { left: 0, top: 0, width: 5120, height: 1600 }
    const secondary = { left: 2560, top: 0, width: 2560, height: 1600 }

    const topLeft = toAbsoluteCoordinates(0, 0, secondary, virtual)
    // 副屏左上角在虚拟桌面的正中间偏右一点。
    expect(topLeft.absX).toBeGreaterThan(32000)
    expect(topLeft.absY).toBe(0)

    const bottomRight = toAbsoluteCoordinates(1, 1, secondary, virtual)
    expect(bottomRight.absX).toBe(65535)
    expect(bottomRight.absY).toBe(65535)
  })

  it('handles a desktop whose origin is negative', () => {
    // 副屏摆在主屏左边时，虚拟桌面原点是负的。按绝对值算会整体偏移一整屏。
    const virtual = { left: -1920, top: 0, width: 4480, height: 1600 }
    const primary = { left: 0, top: 0, width: 2560, height: 1600 }

    const topLeft = toAbsoluteCoordinates(0, 0, primary, virtual)
    expect(topLeft.absX).toBeGreaterThan(27000)
    expect(topLeft.absX).toBeLessThan(29000)
  })

  it('clamps instead of emitting coordinates outside the addressable range', () => {
    // 上游已经钳过一次，这里是最后一道：越界值会被 Windows 当成别的位置。
    const out = toAbsoluteCoordinates(5, -5, singleScreen, singleScreen)
    expect(out.absX).toBe(65535)
    expect(out.absY).toBe(0)
  })

  it('does not divide by zero on a degenerate 1px desktop', () => {
    const tiny = { left: 0, top: 0, width: 1, height: 1 }
    expect(() => toAbsoluteCoordinates(0.5, 0.5, tiny, tiny)).not.toThrow()
    expect(Number.isFinite(toAbsoluteCoordinates(0.5, 0.5, tiny, tiny).absX)).toBe(true)
  })
})
