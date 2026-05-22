import { useCallback, useEffect, useState } from 'react'

interface SamplerStatus {
  enabled: boolean
  paused: boolean
  runtime: {
    running: boolean
    lastL1At: number
    lastL2At: number
    lastError: { label: string; message: string; at: number } | null
    lastWindowTitle: string | null
    lastWindowApp: string | null
  } | null
  activeWinLoadError: string | null
}

/**
 * Tiny topbar pill that shows whether the screen sampler is currently
 * recording, paused, or disabled. Click toggles pause.
 *
 * - Green pulsing dot: sampling
 * - Gray dot:          paused (or disabled)
 * - Red dot:           error (e.g., active-win failed to load)
 *
 * Polls the main-process status every 3s. Cheap (one IPC, fixed shape).
 */
export default function ScreenSamplerIndicator(): JSX.Element | null {
  const [status, setStatus] = useState<SamplerStatus | null>(null)
  const [busy, setBusy] = useState(false)

  const refresh = useCallback(async () => {
    try {
      const s = await window.api.habit.screenSampler.state()
      setStatus(s)
    } catch {
      /* ignore — render falls back to last-known state */
    }
  }, [])

  useEffect(() => {
    void refresh()
    const t = setInterval(() => void refresh(), 3000)
    return () => clearInterval(t)
  }, [refresh])

  const onClick = useCallback(async () => {
    if (busy) return
    setBusy(true)
    try {
      await window.api.habit.screenSampler.togglePause()
      await refresh()
    } finally {
      setBusy(false)
    }
  }, [busy, refresh])

  if (!status || !status.enabled) return null

  const hasError = !!status.activeWinLoadError || !!status.runtime?.lastError
  const active = status.runtime?.running && !status.paused

  let dotClass = 'screen-sampler-dot'
  if (hasError) dotClass += ' error'
  else if (active) dotClass += ' active'
  else dotClass += ' paused'

  let label: string
  if (status.activeWinLoadError) {
    label = `屏幕采集 · 加载失败：${status.activeWinLoadError}`
  } else if (status.paused) {
    label = '屏幕采集 · 已暂停（点击恢复）'
  } else if (status.runtime?.lastError) {
    label = `屏幕采集 · ${status.runtime.lastError.label} 错误（点击暂停）`
  } else if (status.runtime?.lastWindowApp) {
    const win = status.runtime.lastWindowTitle ?? ''
    const trimmed = win.length > 40 ? win.slice(0, 40) + '…' : win
    label = `屏幕采集中 · ${status.runtime.lastWindowApp} · ${trimmed}（点击暂停）`
  } else {
    label = '屏幕采集中（点击暂停）'
  }

  return (
    <button
      type="button"
      className="screen-sampler-indicator"
      title={label}
      onClick={() => void onClick()}
      disabled={busy}
    >
      <span className={dotClass} aria-hidden />
      <span className="screen-sampler-text">
        {status.paused ? '已暂停' : '采集中'}
      </span>
    </button>
  )
}
