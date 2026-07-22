import { describe, expect, it } from 'vitest'
import { chooseScreenshotPngBytes } from '../../../electron/screenshot/exportImage.js'

describe('chooseScreenshotPngBytes', () => {
  it('uses the original cropped native image when the editor has no annotations', () => {
    const original = Buffer.from([1, 2, 3, 4])

    expect(
      chooseScreenshotPngBytes({
        useOriginal: true,
        source: { toPNG: () => original }
      })
    ).toEqual(original)
  })

  it('uses annotated PNG bytes when annotations changed the canvas', () => {
    const annotated = new Uint8Array([9, 8, 7])

    expect(
      chooseScreenshotPngBytes({
        pngBytes: annotated,
        useOriginal: false,
        source: { toPNG: () => Buffer.from([1]) }
      })
    ).toEqual(Buffer.from(annotated))
  })
})
