import { useEffect, useRef, useState } from 'react'
import type { RuntimeState } from '../../electron/preload'
import { canSendRuntimeLog, getRuntimeStatusLabel } from './ProjectBuildPanel.js'
import { scrollRuntimeLogToBottom } from '../utils/runtimeLogViewport.js'
import {
  filterRuntimeLogLines,
  formatRuntimeLogFilterSummary,
} from '../utils/runtimeLogFilter.js'
import {
  clampRuntimeLogFrame,
  createInitialRuntimeLogFrame,
  moveRuntimeLogFrame,
  resizeRuntimeLogFrame,
  type RuntimeLogFloatingFrame,
  type RuntimeLogResizeEdge,
  type RuntimeLogViewportBounds
} from '../utils/runtimeLogFloatingFrame.js'

export interface RuntimeLogDialogProps {
  open: boolean
  currentProjectName: string | null
  currentProjectId: string | null
  runtimeState: RuntimeState
  sessionId: string | null
  sessionStatus: 'idle' | 'running' | 'exited'
  comment: string
  onCommentChange: (next: string) => void
  onClose: () => void
  onStopRuntime: () => void
  onSendRuntimeLog: (comment: string) => void
}

function formatRuntimeMeta(state: RuntimeState): string {
  const parts = [
    state.command ? `命令: ${state.command}` : null,
    state.cwd ? `cwd: ${state.cwd}` : null,
    state.visualStudioDisplayName ? `VS: ${state.visualStudioDisplayName}` : null,
  ].filter(Boolean)
  return parts.length > 0 ? parts.join(' · ') : '等待运行输出'
}

function getViewportBounds(): RuntimeLogViewportBounds {
  if (typeof window === 'undefined') return { width: 1024, height: 768 }
  return { width: window.innerWidth, height: window.innerHeight }
}

type RuntimeLogDragState =
  | {
      kind: 'move'
      pointerX: number
      pointerY: number
      frame: RuntimeLogFloatingFrame
    }
  | {
      kind: 'resize'
      edge: RuntimeLogResizeEdge
      pointerX: number
      pointerY: number
      frame: RuntimeLogFloatingFrame
    }

const resizeEdges: RuntimeLogResizeEdge[] = ['n', 's', 'e', 'w', 'ne', 'nw', 'se', 'sw']

export default function RuntimeLogDialog(props: RuntimeLogDialogProps): JSX.Element | null {
  const logRef = useRef<HTMLPreElement | null>(null)
  const dragStateRef = useRef<RuntimeLogDragState | null>(null)
  const [logFilter, setLogFilter] = useState('')
  const [frame, setFrame] = useState<RuntimeLogFloatingFrame>(() =>
    createInitialRuntimeLogFrame(getViewportBounds())
  )
  const filteredLog = filterRuntimeLogLines(props.runtimeState.log, logFilter)
  const logText = props.runtimeState.log.trim().length > 0
    ? filteredLog.active && filteredLog.text.trim().length === 0
      ? '没有匹配的运行日志'
      : filteredLog.text
    : '暂无运行日志输出'
  const filterSummary = formatRuntimeLogFilterSummary(filteredLog)

  useEffect(() => {
    scrollRuntimeLogToBottom(logRef.current)
  }, [logText])

  useEffect(() => {
    const handleResize = () => {
      setFrame((current) => clampRuntimeLogFrame(current, getViewportBounds()))
    }
    window.addEventListener('resize', handleResize)
    return () => window.removeEventListener('resize', handleResize)
  }, [])

  useEffect(() => {
    const handlePointerMove = (event: PointerEvent) => {
      const active = dragStateRef.current
      if (!active) return
      event.preventDefault()
      const dx = event.clientX - active.pointerX
      const dy = event.clientY - active.pointerY
      const bounds = getViewportBounds()
      setFrame(
        active.kind === 'move'
          ? moveRuntimeLogFrame(active.frame, dx, dy, bounds)
          : resizeRuntimeLogFrame(active.frame, active.edge, dx, dy, bounds)
      )
    }
    const handlePointerUp = () => {
      if (!dragStateRef.current) return
      dragStateRef.current = null
      document.body.style.cursor = ''
      document.body.style.userSelect = ''
    }

    window.addEventListener('pointermove', handlePointerMove)
    window.addEventListener('pointerup', handlePointerUp)
    window.addEventListener('pointercancel', handlePointerUp)
    return () => {
      window.removeEventListener('pointermove', handlePointerMove)
      window.removeEventListener('pointerup', handlePointerUp)
      window.removeEventListener('pointercancel', handlePointerUp)
      document.body.style.cursor = ''
      document.body.style.userSelect = ''
    }
  }, [])

  const startMove = (event: React.PointerEvent<HTMLElement>) => {
    if (event.button !== 0) return
    const target = event.target as HTMLElement | null
    if (target?.closest('button,input,textarea,select,a')) return
    event.preventDefault()
    dragStateRef.current = {
      kind: 'move',
      pointerX: event.clientX,
      pointerY: event.clientY,
      frame
    }
    document.body.style.cursor = 'move'
    document.body.style.userSelect = 'none'
  }

  const startResize = (
    edge: RuntimeLogResizeEdge,
    event: React.PointerEvent<HTMLDivElement>
  ) => {
    if (event.button !== 0) return
    event.preventDefault()
    event.stopPropagation()
    dragStateRef.current = {
      kind: 'resize',
      edge,
      pointerX: event.clientX,
      pointerY: event.clientY,
      frame
    }
    document.body.style.cursor = `${edge}-resize`
    document.body.style.userSelect = 'none'
  }

  if (!props.open) return null

  const sendEnabled = canSendRuntimeLog(
    props.currentProjectId,
    props.runtimeState,
    props.sessionId,
    props.sessionStatus
  )
  const stopEnabled = props.runtimeState.status === 'running'

  return (
    <div className="runtime-log-dialog-layer" role="presentation">
      <section
        className="runtime-log-dialog"
        style={{
          left: `${frame.left}px`,
          top: `${frame.top}px`,
          width: `${frame.width}px`,
          height: `${frame.height}px`
        }}
        role="dialog"
        aria-modal="false"
        aria-label="运行日志"
      >
        <div
          className="runtime-log-dialog-head"
          onPointerDown={startMove}
          data-drag-handle="runtime-log-dialog"
        >
          <div>
            <div className="build-panel-eyebrow">项目运行</div>
            <h2 className="build-panel-title">
              运行日志
              {props.currentProjectName ? (
                <span className="build-panel-project"> · {props.currentProjectName}</span>
              ) : null}
            </h2>
          </div>
          <button className="build-panel-close" onClick={props.onClose} aria-label="关闭运行日志">
            ×
          </button>
        </div>

        <div className="runtime-log-dialog-status">
          <span className={`build-panel-status build-panel-status-${props.runtimeState.status}`}>
            {getRuntimeStatusLabel(props.runtimeState.status)}
          </span>
          <span>{formatRuntimeMeta(props.runtimeState)}</span>
        </div>

        <div className="runtime-log-filter">
          <label htmlFor="runtime-log-filter">过滤日志</label>
          <input
            id="runtime-log-filter"
            value={logFilter}
            onChange={(event) => setLogFilter(event.target.value)}
            placeholder="输入关键字，空格分隔"
          />
          <span>{filterSummary}</span>
        </div>

        <pre ref={logRef} className="runtime-log-dialog-log">{logText}</pre>

        <div className="runtime-log-dialog-comment">
          <label htmlFor="runtime-log-comment">补充问题</label>
          <textarea
            id="runtime-log-comment"
            value={props.comment}
            onChange={(event) => props.onCommentChange(event.target.value)}
            placeholder="例如：帮我分析下这个日志，然后解释下为什么视频没有播放出来。"
            rows={3}
          />
        </div>

        <div className="runtime-log-dialog-actions">
          <button
            className="tile-btn"
            onClick={props.onStopRuntime}
            disabled={!stopEnabled}
            title={stopEnabled ? '停止当前运行进程' : '当前没有正在运行的进程'}
          >
            停止运行
          </button>
          <button
            className="tile-btn primary"
            onClick={() => props.onSendRuntimeLog(props.comment)}
            disabled={!sendEnabled}
            title={sendEnabled ? '发送当前运行日志和补充问题' : '需要运行日志和正在运行的主会话'}
          >
            发送分析
          </button>
        </div>

        {props.runtimeState.log.trim() && (!props.sessionId || props.sessionStatus !== 'running') ? (
          <p className="build-panel-note">主会话未运行，无法发送运行日志。</p>
        ) : null}
        {resizeEdges.map((edge) => (
          <div
            key={edge}
            className={`runtime-log-resize-handle runtime-log-resize-handle-${edge}`}
            onPointerDown={(event) => startResize(edge, event)}
            aria-hidden="true"
          />
        ))}
      </section>
    </div>
  )
}
