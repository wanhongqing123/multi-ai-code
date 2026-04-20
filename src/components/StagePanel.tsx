import { useEffect, useRef, useState } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { SearchAddon } from '@xterm/addon-search'
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
  /** When provided, renders a "📥 导入外部方案" button. */
  onImportExternal?: () => void
  /** When provided (Stage 1 only), renders a "方案预览" button that opens
   *  the current plan markdown for review + annotation → feedback. */
  onReviewPlan?: () => void
  /** When provided (Stage 3 only), renders a "Diff 审查" button that opens
   *  the git diff viewer for annotation → feedback to the Stage 3 CLI. */
  onReviewDiff?: () => void
  /** When provided, renders a "↩ 重新设计" shortcut that sends feedback directly to Stage 1. */
  onRequestRedesign?: () => void
  /** When true, hides the "✓ 完成" manual-done button (e.g. Stage 3 which
   *  signals completion from the CLI itself after user annotations). */
  hideManualDone?: boolean
  /** Plan list for the dropdown selector. Empty array hides nothing — the
   *  "+ 新建方案" sentinel is always available. */
  planList?: { name: string; abs: string; source: 'internal' | 'external' }[]
  /** Currently selected plan name. Empty string means "+ 新建方案" sentinel
   *  is selected (will trigger planPending flow on Start). */
  planName?: string
  /** Called when user changes selection. Value is `__NEW__` for the sentinel
   *  or a plan name from `planList`. */
  onPlanSelect?: (value: string) => void
  /** Per-project CLI overrides from stage config dialog. */
  commandOverride?: string
  envOverride?: Record<string, string>
  /** Project's target repo abs path — used by the MSYS-terminal shortcut (stage ≥ 2). */
  targetRepo?: string
  /** Whether MSYS is enabled for this project. Controls visibility of the 🐚 button. */
  msysEnabled?: boolean
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
  const searchRef = useRef<SearchAddon | null>(null)
  const [showSearch, setShowSearch] = useState(false)
  const [searchQ, setSearchQ] = useState('')
  const unsubRef = useRef<Array<() => void>>([])
  const [status, setStatus] = useState<Status>('idle')
  const [error, setError] = useState<string | null>(null)
  const [dragActive, setDragActive] = useState(false)
  const [startAt, setStartAt] = useState<number | null>(null)
  const [lastOutputAt, setLastOutputAt] = useState<number | null>(null)
  const [bytesOut, setBytesOut] = useState(0)
  const [tick, setTick] = useState(0)
  const lastInputRef = useRef<string>('')
  const inputBufRef = useRef<string>('')
  const [exitInfo, setExitInfo] = useState<{ code: number; signal?: number } | null>(null)

  useEffect(() => {
    if (status !== 'running' && status !== 'awaiting-confirm') return
    const id = setInterval(() => setTick((t) => t + 1), 1000)
    return () => clearInterval(id)
  }, [status])

  // Initialize startAt the first time we flip into running (covers external
  // triggers like advance/startAll that didn't go through handleStart).
  useEffect(() => {
    if (status === 'running' && startAt === null) setStartAt(Date.now())
  }, [status, startAt])

  function fmtDuration(ms: number): string {
    const s = Math.floor(ms / 1000)
    const hh = Math.floor(s / 3600)
    const mm = Math.floor((s % 3600) / 60)
    const ss = s % 60
    if (hh > 0) return `${hh}:${mm.toString().padStart(2, '0')}:${ss.toString().padStart(2, '0')}`
    return `${mm}:${ss.toString().padStart(2, '0')}`
  }

  function fmtBytes(n: number): string {
    if (n < 1024) return `${n}B`
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)}KB`
    return `${(n / (1024 * 1024)).toFixed(2)}MB`
  }

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
      window.api.cc.write(sessionId, data)
      // Accumulate typed characters; commit to lastInputRef on Enter (\r or \n)
      for (const ch of data) {
        if (ch === '\r' || ch === '\n') {
          if (inputBufRef.current.trim().length > 0) {
            lastInputRef.current = inputBufRef.current
          }
          inputBufRef.current = ''
        } else if (ch === '\x7f' || ch === '\b') {
          inputBufRef.current = inputBufRef.current.slice(0, -1)
        } else if (ch >= ' ') {
          inputBufRef.current += ch
        }
      }
    })

    // Intercept copy/paste shortcuts so xterm doesn't forward them as raw keys
    // to the PTY (which would e.g. send Ctrl+C as SIGINT while text is selected).
    term.attachCustomKeyEventHandler((e) => {
      if (e.type !== 'keydown') return true
      const isCopy =
        (e.ctrlKey && e.shiftKey && e.key.toLowerCase() === 'c') ||
        (e.ctrlKey && e.key.toLowerCase() === 'c' && term.hasSelection()) ||
        (e.metaKey && e.key.toLowerCase() === 'c')
      if (isCopy) {
        const sel = term.getSelection()
        if (sel) {
          void navigator.clipboard.writeText(sel)
          term.clearSelection()
        }
        return false
      }
      const isPaste =
        (e.ctrlKey && e.shiftKey && e.key.toLowerCase() === 'v') ||
        (e.metaKey && e.key.toLowerCase() === 'v')
      if (isPaste) {
        void navigator.clipboard
          .readText()
          .then((t) => t && window.api.cc.write(sessionId, t))
        return false
      }
      if ((e.ctrlKey || e.metaKey) && !e.shiftKey && e.key.toLowerCase() === 'f') {
        e.preventDefault()
        setShowSearch(true)
        return false
      }
      return true
    })

    // Right-click context menu: copy selection / paste clipboard
    const onContextMenu = async (e: MouseEvent) => {
      e.preventDefault()
      if (term.hasSelection()) {
        const sel = term.getSelection()
        if (sel) {
          await navigator.clipboard.writeText(sel)
          term.clearSelection()
        }
      } else {
        try {
          const t = await navigator.clipboard.readText()
          if (t) window.api.cc.write(sessionId, t)
        } catch {
          /* clipboard may be empty or permission-denied */
        }
      }
    }
    containerRef.current.addEventListener('contextmenu', onContextMenu)

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
        setLastOutputAt(Date.now())
        setBytesOut((b) => b + (evt.chunk?.length ?? 0))
        setStatus((s) => (s === 'idle' || s === 'exited' ? 'running' : s))
      }
    })
    const offExit = window.api.cc.onExit((evt) => {
      if (evt.sessionId === sessionId) {
        setStatus('exited')
        setExitInfo({ code: evt.exitCode, signal: evt.signal })
        term.write(`\r\n\x1b[33m[process exited code=${evt.exitCode}]\x1b[0m\r\n`)
      }
    })
    const offDone = window.api.stage.onDone((evt) => {
      if (evt.sessionId === sessionId) setStatus('awaiting-confirm')
    })
    unsubRef.current.push(offData, offExit, offDone)

    const hostEl = containerRef.current
    return () => {
      ro.disconnect()
      window.removeEventListener('paste', pasteListener, true)
      hostEl?.removeEventListener('contextmenu', onContextMenu)
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
    setStartAt(Date.now())
    setLastOutputAt(null)
    setBytesOut(0)
    setExitInfo(null)
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
      command: props.commandOverride,
      args,
      env: props.envOverride,
      cols: term.cols,
      rows: term.rows,
      label: props.planName || undefined
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

  async function handleRetry() {
    const lastInput = lastInputRef.current
    await handleStart()
    // Wait for CLI to come up; then replay last user input if any
    if (lastInput.trim()) {
      await new Promise((r) => setTimeout(r, stageId === 1 ? 5500 : 3500))
      window.api.cc.sendUser(sessionId, lastInput)
    }
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
        {props.onImportExternal && (
          <button
            className="tile-btn"
            onClick={props.onImportExternal}
            disabled={disabled}
            title="挑一个外部 .md 文件作为方案。不复制——后续修改直接写回原文件。"
          >
            📥 导入外部方案
          </button>
        )}
        {props.onReviewPlan && (
          <button
            className="tile-btn"
            onClick={props.onReviewPlan}
            disabled={disabled}
            title="预览当前方案并对具体段落做标注，提交后由 Stage 1 CLI 根据意见修改方案"
          >
            👁 方案预览
          </button>
        )}
        {props.onReviewDiff && (
          <button
            className="tile-btn"
            onClick={props.onReviewDiff}
            disabled={disabled}
            title="打开 Git Diff 审查：对代码变更逐行标注，发送后由 Stage 3 CLI 按意见修改代码"
          >
            👁 Diff 审查
          </button>
        )}
        {stageId >= 2 && props.msysEnabled && props.targetRepo && (
          <button
            className="tile-btn"
            onClick={async () => {
              const res = await window.api.shell.openMsysTerminal(props.targetRepo!)
              if (!res.ok) alert(`打开 MSYS 失败：${res.error}`)
            }}
            disabled={disabled}
            title="在当前项目仓库目录打开 MSYS bash 终端，用于手动跑 .sh 脚本"
          >
            🐚 MSYS
          </button>
        )}
        {status === 'idle' || status === 'exited' ? (
          <>
            <button
              className="tile-btn"
              onClick={handleStart}
              disabled={disabled}
              title={disabled ? '请先打开项目' : 'Start'}
            >
              Start
            </button>
            {status === 'exited' && exitInfo && exitInfo.code !== 0 && lastInputRef.current && (
              <button
                className="tile-btn"
                onClick={handleRetry}
                disabled={disabled}
                title={`CLI 非正常退出 (code=${exitInfo.code})。一键重启并重放最后一次输入：\n${lastInputRef.current.slice(0, 80)}…`}
              >
                🔄 重试
              </button>
            )}
          </>
        ) : (
          <>
            {!props.hideManualDone && (
              <button
                className="tile-btn"
                onClick={handleManualDone}
                title="手动标记完成并打开审批抽屉（读取默认产物文件）"
              >
                ✓ 完成
              </button>
            )}
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
      {props.onPlanSelect !== undefined && (
        <div className="plan-name-bar">
          <label>方案选择：</label>
          <select
            value={props.planName ? props.planName : '__NEW__'}
            onChange={(e) => props.onPlanSelect?.(e.target.value)}
            className="plan-name-input"
          >
            <option value="__NEW__">+ 新建方案</option>
            {(props.planList ?? []).map((p) => (
              <option
                key={p.name}
                value={p.name}
                title={p.source === 'external' ? p.abs : ''}
              >
                {p.name}{p.source === 'external' ? '（外部）' : ''}
              </option>
            ))}
          </select>
        </div>
      )}
      {(status === 'running' || status === 'awaiting-confirm') && startAt && (
        <div className="tile-progress" data-tick={tick}>
          <span title="自启动累计时长">⏱ {fmtDuration(Date.now() - startAt)}</span>
          <span
            title="距上次输出的时长；长时间无输出说明 CLI 可能在等待用户、卡住或在长推理中"
            className={
              lastOutputAt && Date.now() - lastOutputAt > 30_000 ? 'stale' : ''
            }
          >
            💤 {lastOutputAt ? fmtDuration(Date.now() - lastOutputAt) : '—'}
          </span>
          <span title="累计输出字节 / 按 ~4 字符 ≈ 1 token 估算">
            📤 {fmtBytes(bytesOut)} ≈ {Math.round(bytesOut / 4 / 1000)}k tok
          </span>
        </div>
      )}
      {error && <div className="tile-error">⚠ {error}</div>}
      {showSearch && (
        <div className="term-search-bar">
          <input
            autoFocus
            className="plan-name-input"
            value={searchQ}
            onChange={(e) => {
              setSearchQ(e.target.value)
              searchRef.current?.findNext(e.target.value, { decorations: { matchBackground: '#5a4400', activeMatchBackground: '#aa7700', matchOverviewRuler: '#5a4400', activeMatchColorOverviewRuler: '#aa7700' } })
            }}
            onKeyDown={(e) => {
              if (e.key === 'Enter') {
                if (e.shiftKey) searchRef.current?.findPrevious(searchQ)
                else searchRef.current?.findNext(searchQ)
              } else if (e.key === 'Escape') {
                setShowSearch(false)
                searchRef.current?.clearDecorations()
              }
            }}
            placeholder="搜索（Enter 下一个 · Shift+Enter 上一个 · Esc 关闭）"
          />
          <button className="tile-btn" onClick={() => searchRef.current?.findPrevious(searchQ)}>↑</button>
          <button className="tile-btn" onClick={() => searchRef.current?.findNext(searchQ)}>↓</button>
          <button
            className="tile-btn"
            onClick={() => {
              setShowSearch(false)
              searchRef.current?.clearDecorations()
            }}
          >
            ×
          </button>
        </div>
      )}
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
