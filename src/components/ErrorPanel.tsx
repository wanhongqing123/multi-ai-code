import { useEffect, useState } from 'react'

export type LogLevel = 'info' | 'warn' | 'error'

export interface LogEntry {
  id: number
  ts: number
  level: LogLevel
  source: string
  message: string
}

let nextId = 1
const listeners = new Set<(log: LogEntry[]) => void>()
let logs: LogEntry[] = []

export function pushLog(level: LogLevel, source: string, message: string): void {
  const entry: LogEntry = { id: nextId++, ts: Date.now(), level, source, message }
  logs = [entry, ...logs].slice(0, 200)
  for (const l of listeners) l(logs)
}

export function useLogs(): [LogEntry[], () => void] {
  const [state, setState] = useState<LogEntry[]>(logs)
  useEffect(() => {
    listeners.add(setState)
    return () => {
      listeners.delete(setState)
    }
  }, [])
  const clear = () => {
    logs = []
    for (const l of listeners) l(logs)
  }
  return [state, clear]
}

/** Helper that logs AND alerts for truly blocking errors. */
export function notifyError(source: string, message: string): void {
  pushLog('error', source, message)
}

/** Helper that just logs. */
export function notifyWarn(source: string, message: string): void {
  pushLog('warn', source, message)
}

export default function ErrorPanel({ onClose }: { onClose: () => void }) {
  const [entries, clear] = useLogs()
  return (
    <div className="error-panel">
      <div className="error-panel-head">
        <span>📣 错误与通知 · {entries.length}</span>
        <span style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
          <button className="drawer-btn" onClick={clear}>
            清空
          </button>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </span>
      </div>
      <div className="error-panel-body">
        {entries.length === 0 ? (
          <div className="drawer-empty">暂无错误或通知</div>
        ) : (
          entries.map((e) => (
            <div key={e.id} className={`error-row lv-${e.level}`}>
              <span className="error-time">{new Date(e.ts).toLocaleTimeString()}</span>
              <span className="error-level">{e.level.toUpperCase()}</span>
              <span className="error-source">{e.source}</span>
              <span className="error-msg">{e.message}</span>
            </div>
          ))
        )}
      </div>
    </div>
  )
}
