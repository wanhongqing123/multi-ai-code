export interface RuntimeLogFloatingFrame {
  left: number
  top: number
  width: number
  height: number
}

export interface RuntimeLogViewportBounds {
  width: number
  height: number
}

export type RuntimeLogResizeEdge =
  | 'n'
  | 's'
  | 'e'
  | 'w'
  | 'ne'
  | 'nw'
  | 'se'
  | 'sw'

const VIEWPORT_MARGIN = 16
const DEFAULT_TOP = 84
const DEFAULT_RIGHT = 20
const DEFAULT_BOTTOM = 20
const DEFAULT_WIDTH = 720
const MIN_WIDTH = 360
const MIN_HEIGHT = 280

function maxUsableWidth(bounds: RuntimeLogViewportBounds): number {
  return Math.max(MIN_WIDTH, bounds.width - VIEWPORT_MARGIN * 2)
}

function maxUsableHeight(bounds: RuntimeLogViewportBounds): number {
  return Math.max(MIN_HEIGHT, bounds.height - VIEWPORT_MARGIN * 2)
}

export function createInitialRuntimeLogFrame(
  bounds: RuntimeLogViewportBounds
): RuntimeLogFloatingFrame {
  const width = Math.min(DEFAULT_WIDTH, maxUsableWidth(bounds))
  const top = Math.min(DEFAULT_TOP, Math.max(VIEWPORT_MARGIN, bounds.height - MIN_HEIGHT))
  const height = Math.min(
    Math.max(MIN_HEIGHT, bounds.height - top - DEFAULT_BOTTOM),
    maxUsableHeight(bounds)
  )
  return clampRuntimeLogFrame(
    {
      left: bounds.width - width - DEFAULT_RIGHT,
      top,
      width,
      height
    },
    bounds
  )
}

export function clampRuntimeLogFrame(
  frame: RuntimeLogFloatingFrame,
  bounds: RuntimeLogViewportBounds
): RuntimeLogFloatingFrame {
  const width = Math.min(Math.max(frame.width, MIN_WIDTH), maxUsableWidth(bounds))
  const height = Math.min(Math.max(frame.height, MIN_HEIGHT), maxUsableHeight(bounds))
  const maxLeft = Math.max(VIEWPORT_MARGIN, bounds.width - width - VIEWPORT_MARGIN)
  const maxTop = Math.max(VIEWPORT_MARGIN, bounds.height - height - VIEWPORT_MARGIN)
  return {
    left: Math.min(Math.max(frame.left, VIEWPORT_MARGIN), maxLeft),
    top: Math.min(Math.max(frame.top, VIEWPORT_MARGIN), maxTop),
    width,
    height
  }
}

export function moveRuntimeLogFrame(
  frame: RuntimeLogFloatingFrame,
  dx: number,
  dy: number,
  bounds: RuntimeLogViewportBounds
): RuntimeLogFloatingFrame {
  return clampRuntimeLogFrame(
    {
      ...frame,
      left: frame.left + dx,
      top: frame.top + dy
    },
    bounds
  )
}

export function resizeRuntimeLogFrame(
  frame: RuntimeLogFloatingFrame,
  edge: RuntimeLogResizeEdge,
  dx: number,
  dy: number,
  bounds: RuntimeLogViewportBounds
): RuntimeLogFloatingFrame {
  let next = { ...frame }

  if (edge.includes('e')) {
    next.width = frame.width + dx
  }
  if (edge.includes('s')) {
    next.height = frame.height + dy
  }
  if (edge.includes('w')) {
    next.left = frame.left + dx
    next.width = frame.width - dx
  }
  if (edge.includes('n')) {
    next.top = frame.top + dy
    next.height = frame.height - dy
  }

  if (next.width < MIN_WIDTH && edge.includes('w')) {
    next.left = frame.left + frame.width - MIN_WIDTH
    next.width = MIN_WIDTH
  }
  if (next.height < MIN_HEIGHT && edge.includes('n')) {
    next.top = frame.top + frame.height - MIN_HEIGHT
    next.height = MIN_HEIGHT
  }

  return clampRuntimeLogFrame(next, bounds)
}
