import { useEffect, useState } from 'react'

const STORAGE_KEY = 'multi-ai-code.templates'

interface Template {
  id: string
  name: string
  body: string
}

function load(): Template[] {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (!raw) return []
    return JSON.parse(raw) as Template[]
  } catch {
    return []
  }
}

function save(list: Template[]): void {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(list))
}

export interface TemplatesDialogProps {
  /** The active session id (if any) to inject into. */
  sessionId?: string | null
  /** Whether the session is currently running (inject enabled only when true). */
  sessionRunning?: boolean
  onClose: () => void
  /** Inject text into the active session via PTY. */
  onInject: (sessionId: string, text: string) => void
}

export default function TemplatesDialog({ sessionId, sessionRunning, onClose, onInject }: TemplatesDialogProps) {
  const [list, setList] = useState<Template[]>([])
  const [editing, setEditing] = useState<Template | null>(null)
  const [form, setForm] = useState({ name: '', body: '' })

  useEffect(() => setList(load()), [])

  function persist(next: Template[]) {
    setList(next)
    save(next)
  }

  function startNew() {
    setEditing({ id: '', name: '', body: '' })
    setForm({ name: '', body: '' })
  }

  function startEdit(t: Template) {
    setEditing(t)
    setForm({ name: t.name, body: t.body })
  }

  function commit() {
    if (!editing) return
    if (!form.name.trim() || !form.body.trim()) return
    let next: Template[]
    if (editing.id) {
      next = list.map((t) =>
        t.id === editing.id ? { ...t, name: form.name.trim(), body: form.body } : t
      )
    } else {
      next = [
        ...list,
        { id: `t_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`, name: form.name.trim(), body: form.body }
      ]
    }
    persist(next)
    setEditing(null)
  }

  function del(id: string) {
    if (!confirm('删除此模板？')) return
    persist(list.filter((t) => t.id !== id))
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal templates-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>📋 Prompt 模板 · {list.length}</h3>
          <button
            className="drawer-btn primary"
            onClick={startNew}
            style={{ marginLeft: 'auto', marginRight: 8 }}
          >
            ＋ 新建
          </button>
          <button className="modal-close" onClick={onClose}>×</button>
        </div>
        <div className="templates-body">
          <aside className="templates-list">
            {list.length === 0 ? (
              <div className="drawer-empty">暂无模板。点「＋ 新建」把常用 prompt 保存进来。</div>
            ) : (
              list.map((t) => (
                <div
                  key={t.id}
                  className={`history-item ${editing?.id === t.id ? 'active' : ''}`}
                  onClick={() => startEdit(t)}
                >
                  <span className="history-time">{t.name}</span>
                  <span className="history-kind">{t.body.length} 字符</span>
                </div>
              ))
            )}
          </aside>
          <section className="templates-detail">
            {editing ? (
              <>
                <label>
                  名称
                  <input
                    className="plan-name-input"
                    value={form.name}
                    onChange={(e) => setForm((f) => ({ ...f, name: e.target.value }))}
                    placeholder="如：继续完善 / 按设计推进实施 / …"
                  />
                </label>
                <label>
                  内容
                  <textarea
                    className="plan-name-input"
                    rows={10}
                    value={form.body}
                    onChange={(e) => setForm((f) => ({ ...f, body: e.target.value }))}
                  />
                </label>
                <div className="drawer-actions" style={{ paddingLeft: 0 }}>
                  <button className="drawer-btn" onClick={() => setEditing(null)}>
                    取消
                  </button>
                  <button className="drawer-btn primary" onClick={commit}>
                    保存
                  </button>
                  {editing.id && (
                    <button className="drawer-btn warn" onClick={() => del(editing.id)}>
                      删除
                    </button>
                  )}
                </div>
                {editing.id && (
                  <div className="templates-inject">
                    <button
                      className="drawer-btn primary"
                      disabled={!sessionRunning || !sessionId}
                      title={!sessionRunning ? '会话未在运行' : undefined}
                      onClick={() => {
                        if (sessionId) {
                          onInject(sessionId, form.body)
                          onClose()
                        }
                      }}
                    >
                      → 注入当前会话
                    </button>
                  </div>
                )}
              </>
            ) : (
              <div className="drawer-empty">从左侧选择或新建一个模板</div>
            )}
          </section>
        </div>
      </div>
    </div>
  )
}
