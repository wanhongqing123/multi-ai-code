import { useEffect, useState } from 'react'
import type { RemoteDesktopControllerState } from '../../electron/remote-desktop/controller.js'

export interface RemoteDesktopSharingBarProps {
  state: RemoteDesktopControllerState
  onStop: () => void
}

function formatDuration(seconds: number): string {
  const mm = Math.floor(seconds / 60)
  const ss = seconds % 60
  return `${String(mm).padStart(2, '0')}:${String(ss).padStart(2, '0')}`
}

/**
 * 共享期常驻指示条。
 *
 * 刻意不提供隐藏入口：无人值守模式下你可能不在电脑前，回来时必须一眼看出
 * 屏幕被看过、现在还在不在被看。这是自动放行之后唯一的可见性保障，
 * 所以它比"界面干净"更重要。
 */
export default function RemoteDesktopSharingBar(
  props: RemoteDesktopSharingBarProps
): JSX.Element | null {
  const sharing = props.state.hostState === 'sharing'
  const [elapsedSeconds, setElapsedSeconds] = useState(0)

  useEffect(() => {
    if (!sharing) {
      setElapsedSeconds(0)
      return
    }
    const startedAt = Date.now()
    const timer = setInterval(() => {
      setElapsedSeconds(Math.floor((Date.now() - startedAt) / 1000))
    }, 1000)
    return () => clearInterval(timer)
  }, [sharing, props.state.sessionId])

  if (!sharing) return null

  return (
    <div className="remote-desktop-sharing-bar" role="status" aria-live="polite">
      <span className="remote-desktop-sharing-bar__dot" aria-hidden="true" />
      <span className="remote-desktop-sharing-bar__text">
        正在共享屏幕给 {props.state.peerUserId ?? '未知设备'} · {formatDuration(elapsedSeconds)}
      </span>
      <button
        type="button"
        className="remote-desktop-sharing-bar__stop"
        onClick={props.onStop}
      >
        停止共享
      </button>
    </div>
  )
}
