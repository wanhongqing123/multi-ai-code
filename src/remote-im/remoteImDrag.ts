export interface RemoteImPanelPosition {
  x: number
  y: number
}

export interface RemoteImPanelFrame {
  viewportWidth: number
  viewportHeight: number
  panelWidth: number
  panelHeight: number
  margin?: number
}

export interface RemoteImDraggedPositionInput {
  startPosition: RemoteImPanelPosition
  startPointer: RemoteImPanelPosition
  currentPointer: RemoteImPanelPosition
  frame: RemoteImPanelFrame
}

const DEFAULT_PANEL_MARGIN = 16
const DEFAULT_PANEL_TOP = 92
const DEFAULT_PANEL_RIGHT = 36

export function clampRemoteImPanelPosition(
  position: RemoteImPanelPosition,
  frame: RemoteImPanelFrame
): RemoteImPanelPosition {
  const margin = frame.margin ?? DEFAULT_PANEL_MARGIN
  const maxX = Math.max(margin, frame.viewportWidth - frame.panelWidth - margin)
  const maxY = Math.max(margin, frame.viewportHeight - frame.panelHeight - margin)
  return {
    x: Math.min(Math.max(position.x, margin), maxX),
    y: Math.min(Math.max(position.y, margin), maxY)
  }
}

export function getInitialRemoteImPanelPosition(frame: RemoteImPanelFrame): RemoteImPanelPosition {
  return clampRemoteImPanelPosition(
    {
      x: frame.viewportWidth - frame.panelWidth - DEFAULT_PANEL_RIGHT,
      y: DEFAULT_PANEL_TOP
    },
    frame
  )
}

export function getDraggedRemoteImPanelPosition(
  input: RemoteImDraggedPositionInput
): RemoteImPanelPosition {
  return clampRemoteImPanelPosition(
    {
      x: input.startPosition.x + input.currentPointer.x - input.startPointer.x,
      y: input.startPosition.y + input.currentPointer.y - input.startPointer.y
    },
    input.frame
  )
}
