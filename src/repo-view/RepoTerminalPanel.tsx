import { useCallback, useEffect, useRef, useState } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import '@xterm/xterm/css/xterm.css'
import { getTheme, THEME_CHANGE_EVENT, type Theme } from '../utils/theme.js'
import {
  buildMainTerminalOptions,
  shouldUseMainTerminalCanvasRenderer,
  xtermThemeFor
} from '../components/mainTerminalConfig.js'
import {
  copySelection,
  installCopyBinding,
  installPasteHandler,
  pasteFromClipboard
} from '../components/terminalClipboard.js'

export interface RepoTerminalPanelProps {
  /** CLI label shown in the header (e.g. "claude"). */
  cliLabel: string
  /** Session running flag, driven by parent from analysis-status events. */
  running: boolean
  /** Parent-supplied starter — same as what "发送给 …" uses, but without a prompt. */
  onStart: () => void
  /** Stops the underlying PTY session. */
  onStop: () => void
}

export default function RepoTerminalPanel(
  props: RepoTerminalPanelProps
): JSX.Element {
  const containerRef = useRef<HTMLDivElement>(null)
  const termRef = useRef<Terminal | null>(null)
  const fitRef = useRef<FitAddon | null>(null)
  const unsubRef = useRef<Array<() => void>>([])
  const [menu, setMenu] = useState<{
    x: number
    y: number
    hasSelection: boolean
  } | null>(null)

  useEffect(() => {
    if (!containerRef.current) return
    const term = new Terminal(buildMainTerminalOptions(getTheme()))
    const onThemeChange = (e: Event) => {
      term.options.theme = xtermThemeFor((e as CustomEvent<Theme>).detail)
    }
    window.addEventListener(THEME_CHANGE_EVENT, onThemeChange)
    const fit = new FitAddon()
    term.loadAddon(fit)
    term.open(containerRef.current)
    if (shouldUseMainTerminalCanvasRenderer()) {
      // Canvas addon is optional; static import only in MainPanel.
    }
    try {
      fit.fit()
    } catch {
      /* ignore */
    }
    termRef.current = term
    fitRef.current = fit

    term.onData((data) => {
      window.api.repoView.analysisInput(data)
    })

    installCopyBinding(term)
    const detachPaste = installPasteHandler(containerRef.current, {
      sessionId: '',
      writeInput: (_sessionId, data) => window.api.repoView.analysisInput(data),
      saveImage: window.api.clipboard.saveImage
    })
    unsubRef.current.push(detachPaste)

    const offData = window.api.repoView.onAnalysisData((evt) => {
      term.write(evt.chunk)
    })
    unsubRef.current.push(offData)

    const ro = new ResizeObserver(() => {
      try {
        fit.fit()
        const { cols, rows } = term
        window.api.repoView.analysisResize(cols, rows)
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
  }, [])

  const handleContextMenu = useCallback(
    (e: React.MouseEvent<HTMLDivElement>) => {
      e.preventDefault()
      const sel = termRef.current?.getSelection() ?? ''
      setMenu({ x: e.clientX, y: e.clientY, hasSelection: sel.length > 0 })
    },
    []
  )

  const closeMenu = useCallback(() => setMenu(null), [])

  const handleMenuCopy = useCallback(() => {
    const term = termRef.current
    if (term) copySelection(term)
    closeMenu()
  }, [closeMenu])

  const handleMenuPaste = useCallback(() => {
    void pasteFromClipboard({
      sessionId: '',
      writeInput: (_sessionId, data) => window.api.repoView.analysisInput(data),
      saveImage: window.api.clipboard.saveImage
    })
    termRef.current?.focus()
    closeMenu()
  }, [closeMenu])

  useEffect(() => {
    if (!menu) return
    const onDown = () => closeMenu()
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') closeMenu()
    }
    window.addEventListener('mousedown', onDown)
    window.addEventListener('resize', closeMenu)
    window.addEventListener('keydown', onKey)
    return () => {
      window.removeEventListener('mousedown', onDown)
      window.removeEventListener('resize', closeMenu)
      window.removeEventListener('keydown', onKey)
    }
  }, [menu, closeMenu])

  return (
    <div className="repo-terminal-panel">
      <div className="repo-terminal-head">
        <span className="repo-terminal-title">AI CLI · {props.cliLabel}</span>
        <div className="repo-terminal-actions">
          {props.running ? (
            <button className="tile-btn" onClick={props.onStop}>
              停止
            </button>
          ) : (
            <button className="tile-btn" onClick={props.onStart}>
              启动
            </button>
          )}
        </div>
      </div>
      <div
        className="repo-terminal-body term-host"
        ref={containerRef}
        onContextMenu={handleContextMenu}
      />
      {menu && (
        <ul
          className="term-ctxmenu"
          style={{ left: menu.x, top: menu.y }}
          onMouseDown={(e) => e.stopPropagation()}
          onContextMenu={(e) => e.preventDefault()}
          role="menu"
        >
          <li
            role="menuitem"
            className={`term-ctxmenu-item${menu.hasSelection ? '' : ' is-disabled'}`}
            onClick={menu.hasSelection ? handleMenuCopy : undefined}
          >
            复制
          </li>
          <li
            role="menuitem"
            className="term-ctxmenu-item"
            onClick={handleMenuPaste}
          >
            粘贴
          </li>
        </ul>
      )}
    </div>
  )
}
