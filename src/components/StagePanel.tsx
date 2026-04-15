import { useEffect, useRef, useState } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import '@xterm/xterm/css/xterm.css'

export interface StagePanelProps {
  stageId: number
  stageName: string
  /** Optional display number for the tile badge (defaults to stageId). */
  displayIndex?: number
  sessionId: string
  projectId: string
  projectDir: string
  cwd: string
  args?: string[]
  autoStart?: boolean
  badgeOverride?: string
  zoomed?: boolean
  hidden?: boolean
  onToggleZoom?: () => void
  /** When this number changes, the panel auto-starts if currently idle/exited. */
  autoStartNonce?: number
  /** When this number changes, the panel kills its session if running. */
  killAllNonce?: number
  /** Opens the reverse-feedback dialog targeting an upstream stage. */
  onRequestFeedback?: () => void
  /** Reports status transitions up to the parent (for aggregated UI state). */
  onStatusChange?: (stageId: number, status: 'idle' | 'running' | 'awaiting-confirm' | 'exited') => void
  /** Disables Start / 完成 / 回退 actions (e.g. no project opened). */
  disabled?: boolean
  /** When provided, renders a "选用历史方案" button that opens the picker. */
  onPickHistory?: () => void
  /** When provided, renders a "↩ 重新设计" shortcut that sends feedback directly to Stage 1. */
  onRequestRedesign?: () => void
}

type Status = 'idle' | 'running' | 'awaiting-confirm' | 'exited'

export default function StagePanel(props: StagePanelProps) {
  const {
    stageId,
    stageName,
    sessionId,
    projectId,
    projectDir,
    cwd,
    args,
    zoomed,
    hidden,
    onToggleZoom,
    autoStartNonce,
    killAllNonce,
    onRequestFeedback,
    onStatusChange,
    disabled
  } = props
  const containerRef = useRef<HTMLDivElement | null>(null)
  const termRef = useRef<Terminal | null>(null)
  const fitRef = useRef<FitAddon | null>(null)
  const unsubRef = useRef<Array<() => void>>([])
  const [status, setStatus] = useState<Status>('idle')
  const [error, setError] = useState<string | null>(null)
  const [dragActive, setDragActive] = useState(false)

  function quoteForShell(p: string): string {
    // Wrap in double quotes if it contains whitespace or special chars.
    return /[\s"'`$&|<>()]/.test(p) ? `"${p.replace(/"/g, '\\"')}"` : p
  }

  function handleDrop(e: React.DragEvent<HTMLDivElement>) {
    e.preventDefault()
    e.stopPropagation()
    setDragActive(false)
    const files = Array.from(e.dataTransfer?.files ?? [])
    if (files.length === 0) return
    const paths: string[] = []
    for (const f of files) {
      try {
        const p = window.api.getPathForFile(f)
        if (p) paths.push(quoteForShell(p))
      } catch {
        /* ignore */
      }
    }
    if (paths.length === 0) return
    // Insert as a space-separated list, with a leading space so it doesn't glue
    // onto any existing prompt text the user just typed.
    const text = (paths.length > 1 ? ' ' : '') + paths.join(' ') + ' '
    window.api.cc.write(sessionId, text)
    termRef.current?.focus()
  }

  function handleDragOver(e: React.DragEvent<HTMLDivElement>) {
    if (!e.dataTransfer?.types?.includes('Files')) return
    e.preventDefault()
    e.stopPropagation()
    e.dataTransfer.dropEffect = 'copy'
    if (!dragActive) setDragActive(true)
  }

  async function handlePaste(e: ClipboardEvent) {
    const host = containerRef.current
    if (!host) return
    // Only intercept if focus is inside this terminal host
    const active = document.activeElement
    if (!active || !host.contains(active)) return
    const items = Array.from(e.clipboardData?.items ?? [])
    const imageItems = items.filter((it) => it.kind === 'file' && it.type.startsWith('image/'))
    if (imageItems.length > 0) {
      e.preventDefault()
      e.stopPropagation()
      const paths: string[] = []
      for (const it of imageItems) {
        const file = it.getAsFile()
        if (!file) continue
        const ext = (file.type.split('/')[1] || 'png').split('+')[0]
        const buf = await file.arrayBuffer()
        const res = await window.api.clipboard.saveImage(buf, ext)
        if (res.ok && res.path) paths.push(quoteForShell(res.path))
      }
      if (paths.length === 0) return
      window.api.cc.write(sessionId, paths.join(' ') + ' ')
      termRef.current?.focus()
      return
    }
    // Plain text paste — xterm.js's internal textarea doesn't always win the
    // paste event on Windows when focus lands on the wrapper div. Write the
    // clipboard text directly to the PTY so Ctrl+V / Cmd+V always works.
    const text = e.clipboardData?.getData('text') ?? ''
    if (!text) return
    e.preventDefault()
    e.stopPropagation()
    window.api.cc.write(sessionId, text)
    termRef.current?.focus()
  }

  function handleDragLeave(e: React.DragEvent<HTMLDivElement>) {
    if (e.currentTarget.contains(e.relatedTarget as Node | null)) return
    setDragActive(false)
  }

  useEffect(() => {
    if (!containerRef.current) return
    const term = new Terminal({
      fontSize: 15,
      lineHeight: 1.2,
      fontFamily: 'Menlo, Monaco, Consolas, "Courier New", monospace',
      cursorBlink: true,
      convertEol: true,
      theme: { background: '#1e1e1e', foreground: '#e6e6e6' },
      allowProposedApi: true
    })
    const fit = new FitAddon()
    term.loadAddon(fit)
    term.open(containerRef.current)
    try {
      fit.fit()
    } catch {
      /* ignore */
    }
    termRef.current = term
    fitRef.current = fit

    term.onData((data) => window.api.cc.write(sessionId, data))

    const ro = new ResizeObserver(() => {
      try {
        fit.fit()
        const { cols, rows } = term
        window.api.cc.resize(sessionId, cols, rows)
      } catch {
        /* ignore */
      }
    })
    ro.observe(containerRef.current)

    const pasteListener = (e: ClipboardEvent) => {
      void handlePaste(e)
    }
    // Capture phase so we beat xterm's textarea from eating the event for images
    window.addEventListener('paste', pasteListener, true)

    const offData = window.api.cc.onData((evt) => {
      if (evt.sessionId === sessionId) {
        term.write(evt.chunk)
        // Auto-flip to running when data flows in (e.g. other panel spawned us)
        setStatus((s) => (s === 'idle' || s === 'exited' ? 'running' : s))
      }
    })
    const offExit = window.api.cc.onExit((evt) => {
      if (evt.sessionId === sessionId) {
        setStatus('exited')
        term.write(`\r\n\x1b[33m[process exited code=${evt.exitCode}]\x1b[0m\r\n`)
      }
    })
    const offDone = window.api.stage.onDone((evt) => {
      if (evt.sessionId === sessionId) setStatus('awaiting-confirm')
    })
    unsubRef.current.push(offData, offExit, offDone)

    return () => {
      ro.disconnect()
      window.removeEventListener('paste', pasteListener, true)
      unsubRef.current.forEach((fn) => fn())
      unsubRef.current = []
      term.dispose()
      termRef.current = null
      fitRef.current = null
    }
  }, [sessionId])

  // External "Start All" trigger
  useEffect(() => {
    if (autoStartNonce === undefined || autoStartNonce === 0) return
    if (status === 'idle' || status === 'exited') {
      handleStart()
    }
    // intentionally ignore status to avoid restart loops on the same nonce
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoStartNonce])

  // External "Kill All" trigger — only our own session; main process already
  // killed it, but we want local state to flip immediately.
  useEffect(() => {
    if (killAllNonce === undefined || killAllNonce === 0) return
    if (status === 'running' || status === 'awaiting-confirm') {
      setStatus('exited')
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [killAllNonce])

  // Report status transitions up to App for aggregated UI state
  useEffect(() => {
    onStatusChange?.(stageId, status)
  }, [status, stageId, onStatusChange])

  // Refit the terminal whenever the zoom/hidden state changes (panel size changes)
  useEffect(() => {
    if (hidden) return
    const id = setTimeout(() => {
      try {
        fitRef.current?.fit()
        const term = termRef.current
        if (term) window.api.cc.resize(sessionId, term.cols, term.rows)
      } catch {
        /* ignore */
      }
    }, 50)
    return () => clearTimeout(id)
  }, [zoomed, hidden, sessionId])

  async function handleStart() {
    setError(null)
    const term = termRef.current
    const fit = fitRef.current
    if (!term || !fit) return
    try {
      fit.fit()
    } catch {
      /* ignore */
    }
    const res = await window.api.cc.spawn({
      sessionId,
      projectId,
      stageId,
      projectDir,
      cwd,
      args,
      cols: term.cols,
      rows: term.rows
    })
    if (!res.ok) {
      setError(res.error ?? 'spawn failed')
      return
    }
    setStatus('running')
  }

  async function handleKill() {
    await window.api.cc.kill(sessionId)
    setStatus('exited')
  }

  async function handleManualDone() {
    setError(null)
    const res = await window.api.stage.triggerDone({
      sessionId,
      projectId,
      stageId,
      projectDir
    })
    if (!res.ok) {
      setError(res.error ?? 'trigger-done failed')
      return
    }
    if (!res.artifactFound) {
      setError(
        `未找到该阶段默认产物文件。请确认 AI 已把结果写入默认路径，或先让它再跑一下 Write。`
      )
    }
  }

  const badge = props.badgeOverride ?? status

  const className = [
    'tile',
    zoomed ? 'tile-zoomed' : '',
    hidden ? 'tile-hidden' : ''
  ]
    .filter(Boolean)
    .join(' ')

  return (
    <section className={className}>
      <div
        className="tile-head"
        onDoubleClick={onToggleZoom}
        title="双击切换放大/还原"
      >
        <span className="tile-id">{props.displayIndex ?? stageId}</span>
        <span className="tile-name">{stageName}</span>
        <span className={`tile-badge ${badge}`}>{badge}</span>
        {props.onPickHistory && (
          <button
            className="tile-btn"
            onClick={props.onPickHistory}
            disabled={disabled}
            title="直接选用此阶段的历史产物或外部文件，跳过 AI 执行"
          >
            📋 选用历史
          </button>
        )}
        {status === 'idle' || status === 'exited' ? (
          <button
            className="tile-btn"
            onClick={handleStart}
            disabled={disabled}
            title={disabled ? '请先打开项目' : 'Start'}
          >
            Start
          </button>
        ) : (
          <>
            <button
              className="tile-btn"
              onClick={handleManualDone}
              title="手动标记完成并打开审批抽屉（读取默认产物文件）"
            >
              ✓ 完成
            </button>
            {onRequestFeedback && (
              <button
                className="tile-btn"
                onClick={onRequestFeedback}
                title="暂停当前阶段并回退到上游（补充方案、修复等）"
              >
                ↺ 回退
              </button>
            )}
            {props.onRequestRedesign && (
              <button
                className="tile-btn"
                onClick={props.onRequestRedesign}
                title="发现方案本身有问题，直接回到「方案设计」阶段重做"
              >
                ⇦ 重新设计
              </button>
            )}
            <button className="tile-btn" onClick={handleKill}>
              Kill
            </button>
          </>
        )}
        {onToggleZoom && (
          <button
            className="tile-btn"
            onClick={onToggleZoom}
            title={zoomed ? '还原' : '放大'}
          >
            {zoomed ? '↙ 还原' : '↗ 放大'}
          </button>
        )}
      </div>
      {error && <div className="tile-error">⚠ {error}</div>}
      <div
        className={`tile-body${dragActive ? ' drag-over' : ''}`}
        onDrop={handleDrop}
        onDragOver={handleDragOver}
        onDragEnter={handleDragOver}
        onDragLeave={handleDragLeave}
      >
        <div ref={containerRef} className="term-host" />
        {dragActive && <div className="drop-hint">松开以粘贴文件路径</div>}
      </div>
    </section>
  )
}
