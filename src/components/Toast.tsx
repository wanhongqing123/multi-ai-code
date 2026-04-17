import { useEffect, useState } from 'react'

export type ToastLevel = 'info' | 'success' | 'warn' | 'error'

export interface ToastItem {
  id: number
  level: ToastLevel
  message: string
  duration?: number
  action?: { label: string; onClick: () => void }
}

let nextId = 1
const listeners = new Set<(toasts: ToastItem[]) => void>()
let toasts: ToastItem[] = []

export function showToast(
  message: string,
  opts: { level?: ToastLevel; duration?: number; action?: ToastItem['action'] } = {}
): number {
  const t: ToastItem = {
    id: nextId++,
    level: opts.level ?? 'info',
    message,
    duration: opts.duration ?? 3000,
    action: opts.action
  }
  toasts = [...toasts, t]
  for (const l of listeners) l(toasts)
  if (t.duration !== 0) {
    setTimeout(() => dismissToast(t.id), t.duration)
  }
  return t.id
}

export function dismissToast(id: number): void {
  toasts = toasts.filter((t) => t.id !== id)
  for (const l of listeners) l(toasts)
}

export default function ToastHost() {
  const [list, setList] = useState<ToastItem[]>(toasts)
  useEffect(() => {
    listeners.add(setList)
    return () => {
      listeners.delete(setList)
    }
  }, [])
  return (
    <div className="toast-host">
      {list.map((t) => (
        <div key={t.id} className={`toast toast-${t.level}`}>
          <span className="toast-msg">{t.message}</span>
          {t.action && (
            <button
              className="toast-action"
              onClick={() => {
                t.action!.onClick()
                dismissToast(t.id)
              }}
            >
              {t.action.label}
            </button>
          )}
          <button className="toast-close" onClick={() => dismissToast(t.id)}>
            ×
          </button>
        </div>
      ))}
    </div>
  )
}
