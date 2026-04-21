import { useCallback, useEffect, useRef, useState } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { SearchAddon } from '@xterm/addon-search'
import '@xterm/xterm/css/xterm.css'
import { getTheme, THEME_CHANGE_EVENT, type Theme } from '../utils/theme.js'

const XTERM_DARK_THEME = {
  background: '#1e1e1e',
  foreground: '#e6e6e6'
}

// Light-theme palette — mirrors the StagePanel colors (Google Blue + status
// colors) so terminal output harmonizes with the rest of the light UI.
const XTERM_LIGHT_THEME = {
  background: '#FFFFFF',
  foreground: '#202124',
  cursor: '#202124',
  cursorAccent: '#FFFFFF',
  selectionBackground: 'rgba(26, 115, 232, 0.2)',
  black: '#202124',
  red: '#D93025',
  green: '#1E8E3E',
  yellow: '#B06000',
  blue: '#1A73E8',
  magenta: '#9334E6',
  cyan: '#0086A3',
  white: '#5F6368',
  brightBlack: '#5F6368',
  brightRed: '#D93025',
  brightGreen: '#1E8E3E',
  brightYellow: '#B06000',
  brightBlue: '#1A73E8',
  brightMagenta: '#9334E6',
  brightCyan: '#0086A3',
  brightWhite: '#202124'
}

function xtermThemeFor(t: Theme): typeof XTERM_DARK_THEME {
  return t === 'dark' ? XTERM_DARK_THEME : XTERM_LIGHT_THEME
}

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
  /** Session running state (driven from App.tsx). */
  status: 'idle' | 'running' | 'exited'
  /** Disabled everything while session is spawning or no project. */
  disabled?: boolean
}

export default function MainPanel(props: MainPanelProps): JSX.Element {
  const containerRef = useRef<HTMLDivElement>(null)
  const termRef = useRef<Terminal | null>(null)
  const fitRef = useRef<FitAddon | null>(null)
  const searchRef = useRef<SearchAddon | null>(null)
  const unsubRef = useRef<Array<() => void>>([])
  const [dragActive, setDragActive] = useState(false)

  useEffect(() => {
    if (!containerRef.current) return
    const term = new Terminal({
      fontSize: 15,
      lineHeight: 1.2,
      fontFamily: 'Menlo, Monaco, Consolas, "Courier New", monospace',
      cursorBlink: true,
      convertEol: true,
      theme: xtermThemeFor(getTheme()),
      allowProposedApi: true
    })
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
      term.write(evt.chunk)
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
