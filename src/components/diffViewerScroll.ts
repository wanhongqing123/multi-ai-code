export function getHorizontalTrackpadDelta(input: {
  deltaX: number
  deltaY: number
  shiftKey: boolean
}): number {
  if (Math.abs(input.deltaX) > 0) return input.deltaX
  if (input.shiftKey && Math.abs(input.deltaY) > 0) return input.deltaY
  return 0
}
