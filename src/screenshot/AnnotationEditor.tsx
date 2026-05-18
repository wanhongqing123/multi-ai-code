import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react'
import {
  ANNOTATION_COLORS,
  ANNOTATION_STROKE_WIDTH,
  DEFAULT_MOSAIC_BLOCK,
  DEFAULT_TEXT_FONT_SIZE,
  appendAnnotation,
  undoLast,
  type Annotation,
  type AnnotationColor,
  type AnnotationTool
} from './annotations'
import { rectFromDrag, type Point } from './rect'

interface EditorPayload {
  imageDataUrl: string
  /** Image-pixel size of the cropped screenshot. */
  size: { w: number; h: number }
}

interface Props {
  sessionToken: string
}

const TOOL_LABELS: Record<AnnotationTool, string> = {
  rect: '矩形',
  arrow: '箭头',
  text: '文本',
  mosaic: '马赛克'
}

/**
 * Renders the full annotation stack onto a target canvas. Used both for live
 * rendering inside the editor and for exporting the final PNG bytes.
 */
function renderAnnotations(
  ctx: CanvasRenderingContext2D,
  baseImage: HTMLImageElement,
  size: { w: number; h: number },
  annotations: Annotation[]
): void {
  ctx.clearRect(0, 0, size.w, size.h)
  ctx.drawImage(baseImage, 0, 0, size.w, size.h)
  for (const a of annotations) {
    if (a.kind === 'rect') {
      ctx.strokeStyle = a.color
      ctx.lineWidth = ANNOTATION_STROKE_WIDTH
      ctx.strokeRect(a.x, a.y, a.w, a.h)
      continue
    }
    if (a.kind === 'arrow') {
      drawArrow(ctx, a.fromX, a.fromY, a.toX, a.toY, a.color)
      continue
    }
    if (a.kind === 'text') {
      ctx.fillStyle = a.color
      ctx.font = `bold ${a.fontSize}px system-ui, sans-serif`
      ctx.textBaseline = 'top'
      // Draw a thin contrasting outline so text is readable on any bg.
      ctx.strokeStyle = 'rgba(255,255,255,0.85)'
      ctx.lineWidth = 3
      ctx.strokeText(a.text, a.x, a.y)
      ctx.fillText(a.text, a.x, a.y)
      continue
    }
    if (a.kind === 'mosaic') {
      drawMosaic(ctx, baseImage, size, a.x, a.y, a.w, a.h, a.block)
    }
  }
}

function drawArrow(
  ctx: CanvasRenderingContext2D,
  fromX: number,
  fromY: number,
  toX: number,
  toY: number,
  color: AnnotationColor
): void {
  const dx = toX - fromX
  const dy = toY - fromY
  const len = Math.hypot(dx, dy)
  if (len < 1) return
  const headLen = Math.min(20, Math.max(10, len * 0.18))
  const angle = Math.atan2(dy, dx)
  ctx.strokeStyle = color
  ctx.fillStyle = color
  ctx.lineWidth = ANNOTATION_STROKE_WIDTH
  ctx.lineCap = 'round'
  ctx.beginPath()
  ctx.moveTo(fromX, fromY)
  ctx.lineTo(toX, toY)
  ctx.stroke()
  // Arrowhead — closed triangle.
  ctx.beginPath()
  ctx.moveTo(toX, toY)
  ctx.lineTo(
    toX - headLen * Math.cos(angle - Math.PI / 7),
    toY - headLen * Math.sin(angle - Math.PI / 7)
  )
  ctx.lineTo(
    toX - headLen * Math.cos(angle + Math.PI / 7),
    toY - headLen * Math.sin(angle + Math.PI / 7)
  )
  ctx.closePath()
  ctx.fill()
}

function drawMosaic(
  ctx: CanvasRenderingContext2D,
  baseImage: HTMLImageElement,
  size: { w: number; h: number },
  rx: number,
  ry: number,
  rw: number,
  rh: number,
  block: number
): void {
  // Sample the underlying image down to a tiny canvas, then scale it back up
  // with image-smoothing off. Cheaper than per-pixel averaging.
  const safeBlock = Math.max(4, Math.floor(block))
  const sw = Math.max(1, Math.floor(rw / safeBlock))
  const sh = Math.max(1, Math.floor(rh / safeBlock))
  const small = document.createElement('canvas')
  small.width = sw
  small.height = sh
  const sctx = small.getContext('2d')
  if (!sctx) return
  // Map the rect in canvas-coords back to image-coords (1:1 here since canvas
  // is sized to image-pixel resolution).
  sctx.imageSmoothingEnabled = false
  sctx.drawImage(baseImage, rx, ry, rw, rh, 0, 0, sw, sh)
  ctx.save()
  ctx.imageSmoothingEnabled = false
  ctx.drawImage(small, 0, 0, sw, sh, rx, ry, rw, rh)
  ctx.restore()
}

export default function AnnotationEditor({ sessionToken }: Props): JSX.Element {
  const canvasRef = useRef<HTMLCanvasElement | null>(null)
  const imageRef = useRef<HTMLImageElement | null>(null)
  const [payload, setPayload] = useState<EditorPayload | null>(null)
  const [tool, setTool] = useState<AnnotationTool>('rect')
  const [color, setColor] = useState<AnnotationColor>(ANNOTATION_COLORS[0])
  const [annotations, setAnnotations] = useState<Annotation[]>([])
  const [drag, setDrag] = useState<{ start: Point; current: Point } | null>(null)
  /**
   * In-flight text annotation. `imageXY` is in image (canvas-coordinate)
   * pixels so the finished annotation lives in the same coord space as the
   * rest; `displayXY` is in CSS pixels relative to the canvas top-left so the
   * floating <input> renders exactly where the user clicked regardless of
   * canvas scale or wrap padding.
   */
  const [pendingText, setPendingText] = useState<{
    imageX: number
    imageY: number
    displayX: number
    displayY: number
    value: string
  } | null>(null)
  const [prompt, setPrompt] = useState('')
  const [sending, setSending] = useState(false)
  const [error, setError] = useState<string | null>(null)

  // Load the cropped image bytes from main.
  useEffect(() => {
    let cancelled = false
    void (async () => {
      const res = await window.api.screenshot.editorLoadPayload(sessionToken)
      if (cancelled || !res.ok) return
      const img = new Image()
      img.onload = () => {
        if (cancelled) return
        imageRef.current = img
        setPayload({ imageDataUrl: res.payload.imageDataUrl, size: res.payload.size })
      }
      img.onerror = () => setError('图片加载失败')
      img.src = res.payload.imageDataUrl
    })()
    return () => {
      cancelled = true
    }
  }, [sessionToken])

  // Repaint whenever annotations or live-drag state changes.
  useLayoutEffect(() => {
    const canvas = canvasRef.current
    const img = imageRef.current
    if (!canvas || !img || !payload) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return
    canvas.width = payload.size.w
    canvas.height = payload.size.h
    renderAnnotations(ctx, img, payload.size, annotations)
    // Live preview of the in-progress drag (not committed yet).
    if (drag) {
      const liveRect = rectFromDrag(drag.start, drag.current, payload.size)
      if (tool === 'rect') {
        ctx.strokeStyle = color
        ctx.lineWidth = ANNOTATION_STROKE_WIDTH
        ctx.strokeRect(liveRect.x, liveRect.y, liveRect.w, liveRect.h)
      } else if (tool === 'arrow') {
        drawArrow(ctx, drag.start.x, drag.start.y, drag.current.x, drag.current.y, color)
      } else if (tool === 'mosaic') {
        drawMosaic(
          ctx,
          img,
          payload.size,
          liveRect.x,
          liveRect.y,
          liveRect.w,
          liveRect.h,
          DEFAULT_MOSAIC_BLOCK
        )
      }
    }
  }, [annotations, drag, payload, color, tool])

  function canvasPoint(e: React.MouseEvent<HTMLCanvasElement>): Point {
    const canvas = canvasRef.current
    if (!canvas || !payload) return { x: 0, y: 0 }
    const rect = canvas.getBoundingClientRect()
    const scaleX = payload.size.w / rect.width
    const scaleY = payload.size.h / rect.height
    return {
      x: (e.clientX - rect.left) * scaleX,
      y: (e.clientY - rect.top) * scaleY
    }
  }

  /** Position relative to canvas top-left in CSS pixels (for floating UI). */
  function displayPoint(e: React.MouseEvent<HTMLCanvasElement>): Point {
    const canvas = canvasRef.current
    if (!canvas) return { x: 0, y: 0 }
    const rect = canvas.getBoundingClientRect()
    return {
      x: e.clientX - rect.left,
      y: e.clientY - rect.top
    }
  }

  function onMouseDown(e: React.MouseEvent<HTMLCanvasElement>) {
    if (!payload || pendingText) return
    const p = canvasPoint(e)
    if (tool === 'text') {
      // Block Chromium's default focus-shift from this mousedown — otherwise
      // it fires AFTER React's commit and immediately blurs the freshly
      // autofocused <input>, triggering onBlur → commitText → setPendingText(null)
      // within one frame. To the user it looks like nothing happened.
      e.preventDefault()
      const d = displayPoint(e)
      setPendingText({
        imageX: p.x,
        imageY: p.y,
        displayX: d.x,
        displayY: d.y,
        value: ''
      })
      return
    }
    setDrag({ start: p, current: p })
  }

  function onMouseMove(e: React.MouseEvent<HTMLCanvasElement>) {
    if (!drag) return
    setDrag({ start: drag.start, current: canvasPoint(e) })
  }

  function onMouseUp(e: React.MouseEvent<HTMLCanvasElement>) {
    if (!drag || !payload) return
    const end = canvasPoint(e)
    const liveRect = rectFromDrag(drag.start, end, payload.size)
    setDrag(null)
    if (tool === 'rect') {
      if (liveRect.w < 4 || liveRect.h < 4) return
      setAnnotations((a) =>
        appendAnnotation(a, { kind: 'rect', color, ...liveRect })
      )
    } else if (tool === 'arrow') {
      const dist = Math.hypot(end.x - drag.start.x, end.y - drag.start.y)
      if (dist < 6) return
      setAnnotations((a) =>
        appendAnnotation(a, {
          kind: 'arrow',
          color,
          fromX: drag.start.x,
          fromY: drag.start.y,
          toX: end.x,
          toY: end.y
        })
      )
    } else if (tool === 'mosaic') {
      if (liveRect.w < 6 || liveRect.h < 6) return
      setAnnotations((a) =>
        appendAnnotation(a, {
          kind: 'mosaic',
          ...liveRect,
          block: DEFAULT_MOSAIC_BLOCK
        })
      )
    }
  }

  function commitText() {
    if (!pendingText) return
    const trimmed = pendingText.value.trim()
    if (trimmed) {
      setAnnotations((a) =>
        appendAnnotation(a, {
          kind: 'text',
          color,
          x: pendingText.imageX,
          y: pendingText.imageY,
          fontSize: DEFAULT_TEXT_FONT_SIZE,
          text: trimmed
        })
      )
    }
    setPendingText(null)
  }

  const handleUndo = useCallback(() => {
    setAnnotations((a) => undoLast(a))
  }, [])

  useEffect(() => {
    function onKey(e: KeyboardEvent) {
      const meta = e.ctrlKey || e.metaKey
      if (meta && e.key.toLowerCase() === 'z') {
        e.preventDefault()
        handleUndo()
      } else if (e.key === 'Escape' && !pendingText) {
        void window.api.screenshot.editorCancel(sessionToken)
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [handleUndo, pendingText, sessionToken])

  async function handleSend() {
    if (!payload || sending) return
    const canvas = canvasRef.current
    if (!canvas) return
    setSending(true)
    setError(null)
    try {
      // Flatten any uncommitted text first.
      if (pendingText) commitText()
      const blob: Blob | null = await new Promise((r) => canvas.toBlob(r, 'image/png'))
      if (!blob) throw new Error('canvas.toBlob returned null')
      const bytes = new Uint8Array(await blob.arrayBuffer())
      const res = await window.api.screenshot.editorSend({
        token: sessionToken,
        pngBytes: bytes,
        prompt
      })
      if (!res.ok) throw new Error(res.error)
    } catch (err) {
      setError((err as Error).message)
      setSending(false)
    }
  }

  if (error && !payload) {
    return <div className="screenshot-editor-error">加载失败：{error}</div>
  }
  if (!payload) {
    return <div className="screenshot-editor-loading">加载截图…</div>
  }

  return (
    <div className="screenshot-editor-root">
      <header className="screenshot-editor-toolbar">
        <div className="screenshot-tool-group">
          {(Object.keys(TOOL_LABELS) as AnnotationTool[]).map((t) => (
            <button
              key={t}
              type="button"
              className={`screenshot-tool-btn ${tool === t ? 'active' : ''}`}
              onClick={() => setTool(t)}
            >
              {TOOL_LABELS[t]}
            </button>
          ))}
        </div>
        <div className="screenshot-color-group">
          {ANNOTATION_COLORS.map((c) => (
            <button
              key={c}
              type="button"
              aria-label={`color ${c}`}
              className={`screenshot-color-swatch ${color === c ? 'active' : ''}`}
              style={{ background: c }}
              onClick={() => setColor(c)}
            />
          ))}
        </div>
        <div className="screenshot-action-group">
          <button
            type="button"
            className="screenshot-tool-btn"
            disabled={annotations.length === 0}
            onClick={handleUndo}
            title="撤销 (Ctrl/Cmd+Z)"
          >
            撤销
          </button>
          <button
            type="button"
            className="screenshot-tool-btn"
            onClick={() => void window.api.screenshot.editorCancel(sessionToken)}
            title="放弃这张截图"
          >
            取消
          </button>
        </div>
      </header>

      <div className="screenshot-canvas-wrap">
        {/* The anchor div hugs the canvas exactly so px-offset overlays
            (currently just the in-flight text input) sit flush with where
            the user clicked, regardless of how the wrap centers/pads us. */}
        <div className="screenshot-canvas-anchor">
          <canvas
            ref={canvasRef}
            className="screenshot-canvas"
            onMouseDown={onMouseDown}
            onMouseMove={onMouseMove}
            onMouseUp={onMouseUp}
            onMouseLeave={(e) => {
              if (drag) onMouseUp(e as unknown as React.MouseEvent<HTMLCanvasElement>)
            }}
          />
          {pendingText && (
            <input
              autoFocus
              className="screenshot-text-input"
              value={pendingText.value}
              placeholder="输入文字，回车提交"
              onChange={(e) =>
                setPendingText(
                  pendingText ? { ...pendingText, value: e.target.value } : null
                )
              }
              onKeyDown={(e) => {
                if (e.key === 'Enter') {
                  e.preventDefault()
                  commitText()
                } else if (e.key === 'Escape') {
                  e.preventDefault()
                  setPendingText(null)
                }
              }}
              onBlur={() => commitText()}
              style={{
                left: pendingText.displayX,
                top: pendingText.displayY,
                color
              }}
            />
          )}
        </div>
      </div>

      <footer className="screenshot-editor-footer">
        <textarea
          className="screenshot-prompt-input"
          placeholder="附带一句话发给 AI（比如：这个错误怎么修？）"
          rows={2}
          value={prompt}
          onChange={(e) => setPrompt(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
              e.preventDefault()
              void handleSend()
            }
          }}
        />
        <div className="screenshot-send-actions">
          {error && <span className="screenshot-send-error">{error}</span>}
          <button
            type="button"
            className="screenshot-send-btn"
            disabled={sending}
            onClick={() => void handleSend()}
            title="Ctrl/Cmd+Enter"
          >
            {sending ? '发送中…' : '发送到主会话'}
          </button>
        </div>
      </footer>
    </div>
  )
}
