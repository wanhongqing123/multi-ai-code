import { useCallback, useEffect, useRef, useState } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { SearchAddon } from '@xterm/addon-search'
import { CanvasAddon } from '@xterm/addon-canvas'
import '@xterm/xterm/css/xterm.css'
import { getTheme, THEME_CHANGE_EVENT, type Theme } from '../utils/theme.js'
import {
  buildMainTerminalOptions,
  shouldUseMainTerminalCanvasRenderer,
  shouldEnableMainTerminalGpuAcceleration,
  xtermThemeFor
} from './mainTerminalConfig.js'
import {
  createTerminalMarkdownState,
  formatMarkdownChunk
} from './terminalMarkdown.js'

export interface MainPanelProps {
  sessionId: string
  projectId: string
  projectDir: string
  /** cwd = target_repo. */
  cwd: string
  /** Current plan name. */
  planName: string
  /** Called when user clicks Start. */
  onStart: () => void
  /** Called when user clicks Stop. */
  onStop: () => void
  /** Called when user clicks Restart. */
  onRestart: () => void
  /** Called when user clicks "Diff 审查". */
  onOpenDiff: () => void
  /** Called when user clicks "仓库查看". */
  onOpenRepoView: () => void
  /** Session running state (driven from App.tsx). */
  status: 'idle' | 'running' | 'exited'
  /** Disabled everything while session is spawning or no project. */
  disabled?: boolean
  /** Disable only the repo-view button. */
  repoViewDisabled?: boolean
}

export default function MainPanel(props: MainPanelProps): JSX.Element {
  const containerRef = useRef<HTMLDivElement>(null)
  const termRef = useRef<Terminal | null>(null)
  const fitRef = useRef<FitAddon | null>(null)
  const searchRef = useRef<SearchAddon | null>(null)
  const markdownStateRef = useRef(createTerminalMarkdownState())
  const unsubRef = useRef<Array<() => void>>([])
  const [dragActive, setDragActive] = useState(false)

  useEffect(() => {
    if (!containerRef.current) return
    const term = new Terminal(buildMainTerminalOptions(getTheme()))
    const onThemeChange = (e: Event) => {
      term.options.theme = xtermThemeFor((e as CustomEvent<Theme>).detail)
    }
    window.addEventListener(THEME_CHANGE_EVENT, onThemeChange)
    const fit = new FitAddon()
    const search = new SearchAddon()
    term.loadAddon(fit)
    term.loadAddon(search)
    searchRef.current = search
    term.open(containerRef.current)
    if (shouldUseMainTerminalCanvasRenderer()) {
      term.loadAddon(new CanvasAddon())
    }
    if (shouldEnableMainTerminalGpuAcceleration()) {
      /* reserved for a future opt-in path after Electron stability work */
    }
    try {
      fit.fit()
    } catch {
      /* ignore */
    }
    termRef.current = term
    fitRef.current = fit

    term.onData((data) => {
      window.api.cc.write(props.sessionId, data)
    })

    const offData = window.api.cc.onData((evt) => {
      if (evt.sessionId !== props.sessionId) return
      const formatted = formatMarkdownChunk(evt.chunk, markdownStateRef.current)
      term.write(formatted.text)
    })
    unsubRef.current.push(offData)

    const offExit = window.api.cc.onExit((evt) => {
      if (evt.sessionId !== props.sessionId) return
      // status transition is handled in App.tsx via the same event
    })
    unsubRef.current.push(offExit)

    const ro = new ResizeObserver(() => {
      try {
        fit.fit()
        const { cols, rows } = term
        window.api.cc.resize(props.sessionId, cols, rows)
      } catch {
        /* ignore */
      }
    })
    ro.observe(containerRef.current)

    return () => {
      ro.disconnect()
      window.removeEventListener(THEME_CHANGE_EVENT, onThemeChange)
      unsubRef.current.forEach((fn) => fn())
      unsubRef.current = []
      term.dispose()
      termRef.current = null
      fitRef.current = null
      markdownStateRef.current = createTerminalMarkdownState()
    }
  }, [props.sessionId])

  const handleDragOver = useCallback((e: React.DragEvent<HTMLDivElement>) => {
    e.preventDefault()
    setDragActive(true)
  }, [])
  const handleDragLeave = useCallback(
    (e: React.DragEvent<HTMLDivElement>) => {
      if (e.currentTarget.contains(e.relatedTarget as Node | null)) return
      setDragActive(false)
    },
    []
  )
  const handleDrop = useCallback(
    (e: React.DragEvent<HTMLDivElement>) => {
      e.preventDefault()
      setDragActive(false)
      const files = Array.from(e.dataTransfer.files)
      if (files.length === 0) return
      const paths = files
        .map((f) => (f as unknown as { path: string }).path)
        .filter(Boolean)
      if (paths.length === 0) return
      window.api.cc.write(props.sessionId, paths.join(' '))
      termRef.current?.focus()
    },
    [props.sessionId]
  )

  const statusLabel =
    props.status === 'running'
      ? '运行中'
      : props.status === 'exited'
        ? '已退出'
        : '待启动'

  return (
    <div className="main-panel">
      <div className="main-panel-head">
        <div className="main-panel-title">
          <span className="main-panel-plan">
            {props.planName || '(未选择方案)'}
          </span>
          <span className={`tile-badge ${props.status}`}>{statusLabel}</span>
        </div>
        <div className="main-panel-actions">
          <button
            className="tile-btn"
            onClick={props.onOpenRepoView}
            disabled={props.repoViewDisabled}
            title="打开独立仓库查看窗口"
          >
            仓库查看
          </button>
          <button
            className="tile-btn"
            onClick={props.onOpenDiff}
            disabled={props.disabled || props.status !== 'running'}
            title="打开 Diff 审查（把批注回灌给当前会话）"
          >
            Diff 审查
          </button>
          {props.status === 'running' ? (
            <button
              className="tile-btn"
              onClick={props.onStop}
              disabled={props.disabled}
            >
              停止
            </button>
          ) : (
            <button
              className="tile-btn"
              onClick={
                props.status === 'exited' ? props.onRestart : props.onStart
              }
              disabled={props.disabled}
            >
              {props.status === 'exited' ? '重启' : '启动'}
            </button>
          )}
        </div>
      </div>
      <div
        className="main-panel-body term-host"
        ref={containerRef}
        onDragOver={handleDragOver}
        onDragLeave={handleDragLeave}
        onDrop={handleDrop}
      >
        {dragActive && <div className="drop-hint">松开以粘贴文件路径</div>}
      </div>
    </div>
  )
}
