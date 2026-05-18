import { describe, expect, it } from 'vitest'
import { isUsableRect, rectFromDrag, scaleRect } from './rect.js'

describe('rectFromDrag', () => {
  const bounds = { w: 1000, h: 800 }

  it('builds a positive rect from a top-left → bottom-right drag', () => {
    expect(rectFromDrag({ x: 10, y: 20 }, { x: 110, y: 220 }, bounds)).toEqual({
      x: 10,
      y: 20,
      w: 100,
      h: 200
    })
  })

  it('normalizes an inverted (bottom-right → top-left) drag', () => {
    expect(rectFromDrag({ x: 110, y: 220 }, { x: 10, y: 20 }, bounds)).toEqual({
      x: 10,
      y: 20,
      w: 100,
      h: 200
    })
  })

  it('clamps to bounds on edges', () => {
    expect(rectFromDrag({ x: -50, y: -50 }, { x: 2000, y: 2000 }, bounds)).toEqual({
      x: 0,
      y: 0,
      w: 1000,
      h: 800
    })
  })

  it('produces zero w/h for a zero-length drag', () => {
    expect(rectFromDrag({ x: 50, y: 50 }, { x: 50, y: 50 }, bounds)).toEqual({
      x: 50,
      y: 50,
      w: 0,
      h: 0
    })
  })
})

describe('isUsableRect', () => {
  it('rejects rects below the minimum size threshold', () => {
    expect(isUsableRect({ x: 0, y: 0, w: 5, h: 5 })).toBe(false)
    expect(isUsableRect({ x: 0, y: 0, w: 50, h: 5 })).toBe(false)
  })

  it('accepts rects at or above the threshold', () => {
    expect(isUsableRect({ x: 0, y: 0, w: 10, h: 10 })).toBe(true)
    expect(isUsableRect({ x: 0, y: 0, w: 200, h: 100 })).toBe(true)
  })
})

describe('scaleRect', () => {
  it('multiplies x/y/w/h by separate axis scales', () => {
    expect(scaleRect({ x: 10, y: 20, w: 100, h: 200 }, 2, 3)).toEqual({
      x: 20,
      y: 60,
      w: 200,
      h: 600
    })
  })

  it('rounds fractional results', () => {
    expect(scaleRect({ x: 1, y: 1, w: 3, h: 3 }, 1.5, 1.5)).toEqual({
      x: 2,
      y: 2,
      w: 5,
      h: 5
    })
  })
})
