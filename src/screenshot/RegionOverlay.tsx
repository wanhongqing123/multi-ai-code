import { useCallback, useEffect, useRef, useState } from 'react'
import { isUsableRect, rectFromDrag, type Point, type Rect } from './rect'

interface OverlayPayload {
  /** PNG data URL of the captured screen. */
  imageDataUrl: string
  /** Logical (CSS-pixel) size of the captured display. */
  logicalSize: { w: number; h: number }
  /** Physical (image-pixel) size of the captured display. */
  physicalSize: { w: number; h: number }
}

interface Props {
  sessionToken: string
}

/**
 * Fullscreen-overlay window that lets the user drag a rectangular region on
 * top of the most recently captured screen. On commit, the rect is reported
 * back to the main process (which crops the image and opens the annotation
 * editor). Pressing Esc or right-clicking cancels.
 */
export default function RegionOverlay({ sessionToken }: Props): JSX.Element {
  const [payload, setPayload] = useState<OverlayPayload | null>(null)
  const [start, setStart] = useState<Point | null>(null)
  const [end, setEnd] = useState<Point | null>(null)
  const draggingRef = useRef(false)

  useEffect(() => {
    let cancelled = false
    void (async () => {
      const data = await window.api.screenshot.overlayLoadPayload(sessionToken)
      if (!cancelled && data.ok) setPayload(data.payload)
    })()
    return () => {
      cancelled = true
    }
  }, [sessionToken])

  const handleCancel = useCallback(() => {
    void window.api.screenshot.overlayCancel(sessionToken)
  }, [sessionToken])

  const handleCommit = useCallback(
    (logicalRect: Rect) => {
      if (!payload) return
      void window.api.screenshot.overlayCommit({
        token: sessionToken,
        logicalRect,
        logicalSize: payload.logicalSize,
        physicalSize: payload.physicalSize
      })
    },
    [payload, sessionToken]
  )

  useEffect(() => {
    function onKey(e: KeyboardEvent) {
      if (e.key === 'Escape') {
        e.preventDefault()
        handleCancel()
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [handleCancel])

  if (!payload) {
    return (
      <div className="screenshot-overlay-loading" aria-label="Capturing screen…" />
    )
  }

  const bounds = payload.logicalSize
  const liveRect = start && end ? rectFromDrag(start, end, bounds) : null

  function onMouseDown(e: React.MouseEvent) {
    if (e.button !== 0) {
      handleCancel()
      return
    }
    draggingRef.current = true
    setStart({ x: e.clientX, y: e.clientY })
    setEnd({ x: e.clientX, y: e.clientY })
  }

  function onMouseMove(e: React.MouseEvent) {
    if (!draggingRef.current) return
    setEnd({ x: e.clientX, y: e.clientY })
  }

  function onMouseUp(e: React.MouseEvent) {
    if (!draggingRef.current) return
    draggingRef.current = false
    const final = rectFromDrag(start ?? { x: 0, y: 0 }, { x: e.clientX, y: e.clientY }, bounds)
    if (isUsableRect(final)) {
      handleCommit(final)
    } else {
      // Tiny drag: treat as cancel + restart.
      setStart(null)
      setEnd(null)
    }
  }

  function onContextMenu(e: React.MouseEvent) {
    e.preventDefault()
    handleCancel()
  }

  return (
    <div
      className="screenshot-overlay-root"
      style={{ width: bounds.w, height: bounds.h }}
      onMouseDown={onMouseDown}
      onMouseMove={onMouseMove}
      onMouseUp={onMouseUp}
      onContextMenu={onContextMenu}
    >
      <img
        className="screenshot-overlay-bg"
        src={payload.imageDataUrl}
        alt=""
        draggable={false}
        style={{ width: bounds.w, height: bounds.h }}
      />
      <div className="screenshot-overlay-dim" />
      {liveRect && liveRect.w > 0 && liveRect.h > 0 && (
        <>
          <div
            className="screenshot-overlay-cutout"
            style={{
              left: liveRect.x,
              top: liveRect.y,
              width: liveRect.w,
              height: liveRect.h,
              backgroundImage: `url(${payload.imageDataUrl})`,
              backgroundPosition: `-${liveRect.x}px -${liveRect.y}px`,
              backgroundSize: `${bounds.w}px ${bounds.h}px`
            }}
          />
          <div
            className="screenshot-overlay-rect-border"
            style={{
              left: liveRect.x,
              top: liveRect.y,
              width: liveRect.w,
              height: liveRect.h
            }}
          />
          <div
            className="screenshot-overlay-size-tag"
            style={{
              left: liveRect.x,
              top: Math.max(0, liveRect.y - 24)
            }}
          >
            {liveRect.w} × {liveRect.h}
          </div>
        </>
      )}
      <div className="screenshot-overlay-hint">
        拖动选择截图区域 · 松开鼠标确认 · Esc / 右键取消
      </div>
    </div>
  )
}
