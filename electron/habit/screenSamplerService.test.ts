import { describe, expect, it } from 'vitest'
import { resolveCaptureRequestSize } from './screenSamplerService.js'

describe('resolveCaptureRequestSize', () => {
  it('prefers the primary display physical size over the old thumbnail target', () => {
    expect(
      resolveCaptureRequestSize(
        {
          size: { width: 1920, height: 1080 },
          scaleFactor: 1.5
        },
        1280,
        720
      )
    ).toEqual({
      width: 2880,
      height: 1620
    })
  })

  it('keeps the requested size as a floor when the display metadata is smaller', () => {
    expect(
      resolveCaptureRequestSize(
        {
          size: { width: 800, height: 600 },
          scaleFactor: 1
        },
        1280,
        720
      )
    ).toEqual({
      width: 1280,
      height: 720
    })
  })
})
