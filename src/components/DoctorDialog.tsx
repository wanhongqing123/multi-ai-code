import { useEffect, useState } from 'react'

interface DoctorResult {
  name: string
  required: boolean
  ok: boolean
  version?: string
  error?: string
  install: string
}

export interface DoctorDialogProps {
  onClose: () => void
  /** Called with the latest results on mount and after rechecking. */
  onResults?: (results: DoctorResult[]) => void
}

export default function DoctorDialog({ onClose, onResults }: DoctorDialogProps) {
  const [results, setResults] = useState<DoctorResult[] | null>(null)
  const [checking, setChecking] = useState(false)

  async function run() {
    setChecking(true)
    const r = await window.api.doctor.check()
    setResults(r)
    onResults?.(r)
    setChecking(false)
  }

  useEffect(() => {
    void run()
  }, [])

  const failed = results?.filter((r) => !r.ok) ?? []

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal doctor-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>
            🩺 CLI 健康检查
            {results && (failed.length === 0
              ? <span style={{ color: '#7fd67f', marginLeft: 8 }}>· 全部就绪 ✓</span>
              : <span style={{ color: '#f88', marginLeft: 8 }}>· {failed.length} 项未就绪</span>)}
          </h3>
          <button
            className="drawer-btn"
            onClick={run}
            disabled={checking}
            style={{ marginLeft: 'auto', marginRight: 8 }}
          >
            {checking ? '检测中…' : '↻ 重检'}
          </button>
          <button className="modal-close" onClick={onClose}>×</button>
        </div>
        <div className="doctor-body">
          {!results ? (
            <div className="drawer-empty">检测中，请稍候…</div>
          ) : (
            <table className="doctor-table">
              <thead>
                <tr>
                  <th style={{ width: 90 }}>工具</th>
                  <th style={{ width: 60 }}>状态</th>
                  <th>版本 / 错误</th>
                </tr>
              </thead>
              <tbody>
                {results.map((r) => (
                  <tr key={r.name}>
                    <td>
                      <code>{r.name}</code>
                      {r.required && <span className="doctor-req">必需</span>}
                    </td>
                    <td>
                      {r.ok ? (
                        <span style={{ color: '#7fd67f' }}>✓ OK</span>
                      ) : (
                        <span style={{ color: '#f88' }}>✗ 缺失</span>
                      )}
                    </td>
                    <td>
                      {r.ok ? (
                        <code style={{ color: '#aad' }}>{r.version}</code>
                      ) : (
                        <>
                          <div style={{ color: '#f88', fontSize: 11 }}>{r.error}</div>
                          <div style={{ color: '#cde', fontSize: 11, marginTop: 4 }}>
                            ↳ {r.install}
                          </div>
                        </>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      </div>
    </div>
  )
}
