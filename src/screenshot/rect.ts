export interface Rect {
  x: number
  y: number
  w: number
  h: number
}

export interface Point {
  x: number
  y: number
}

/**
 * Build a positive-w/h rect from a (possibly inverted) start→end drag.
 * Coordinates are clamped to the [0, bounds.w/h] box so a drag that strays
 * past the screen edge still produces a sane crop.
 */
export function rectFromDrag(
  start: Point,
  end: Point,
  bounds: { w: number; h: number }
): Rect {
  const x0 = Math.max(0, Math.min(start.x, end.x))
  const y0 = Math.max(0, Math.min(start.y, end.y))
  const x1 = Math.min(bounds.w, Math.max(start.x, end.x))
  const y1 = Math.min(bounds.h, Math.max(start.y, end.y))
  return {
    x: Math.round(x0),
    y: Math.round(y0),
    w: Math.max(0, Math.round(x1 - x0)),
    h: Math.max(0, Math.round(y1 - y0))
  }
}

/** A region drag is considered valid only if it's at least this large. */
export const MIN_REGION_SIZE_PX = 10

export function isUsableRect(rect: Rect): boolean {
  return rect.w >= MIN_REGION_SIZE_PX && rect.h >= MIN_REGION_SIZE_PX
}

/**
 * Convert a logical CSS-pixel rect (from the overlay window which is at the
 * display's logical resolution) into the source-image pixel rect (which is at
 * the physical resolution). desktopCapturer.thumbnailSize maps to physical
 * pixels on retina-style displays.
 */
export function scaleRect(rect: Rect, scaleX: number, scaleY: number): Rect {
  return {
    x: Math.round(rect.x * scaleX),
    y: Math.round(rect.y * scaleY),
    w: Math.round(rect.w * scaleX),
    h: Math.round(rect.h * scaleY)
  }
}
