export interface ScreenshotPngSource {
  toPNG(): Buffer | Uint8Array
}

export function chooseScreenshotPngBytes(input: {
  pngBytes?: Uint8Array | ArrayBuffer | null
  useOriginal?: boolean
  source?: ScreenshotPngSource | null
}): Buffer {
  if (input.useOriginal) {
    if (!input.source) throw new Error('original screenshot source is missing')
    return Buffer.from(input.source.toPNG())
  }
  if (!input.pngBytes) throw new Error('annotated screenshot bytes are missing')
  return Buffer.from(
    input.pngBytes instanceof Uint8Array
      ? input.pngBytes
      : new Uint8Array(input.pngBytes)
  )
}
