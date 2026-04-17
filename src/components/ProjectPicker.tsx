import { useEffect, useState } from 'react'
import { showToast } from './Toast'

export interface ProjectInfo {
  id: string
  name: string
  target_repo: string
  dir: string
  created_at: string
  updated_at: string
}

export interface ProjectPickerProps {
  currentId: string | null
  onClose: () => void
  onSelect: (p: ProjectInfo) => void
  /** Called after a project is created / deleted — parent should refresh. */
  onChanged: () => void
}

export default function ProjectPicker({
  currentId,
  onClose,
  onSelect,
  onChanged
}: ProjectPickerProps) {
  const [projects, setProjects] = useState<ProjectInfo[]>([])
  const [busy, setBusy] = useState(false)

  async function reload() {
    const list = await window.api.project.list()
    setProjects(list)
  }

  useEffect(() => {
    void reload()
  }, [])

  async function handleCreate() {
    const pick = await window.api.project.pickDir()
    if (pick.canceled || !pick.path) return
    const name = window.prompt(
      '请输入项目名称（用于在列表中识别）：',
      pick.path.split(/[/\\]/).filter(Boolean).pop() ?? ''
    )
    if (!name?.trim()) return
    setBusy(true)
    try {
      const res = await window.api.project.create(name.trim(), pick.path)
      if (!res.ok) {
        alert(`新建项目失败：${res.error}`)
        return
      }
      await reload()
      onChanged()
      if (res.id) {
        onSelect({
          id: res.id,
          name: res.name!,
          target_repo: res.target_repo!,
          dir: res.dir!,
          created_at: new Date().toISOString(),
          updated_at: new Date().toISOString()
        })
      }
    } finally {
      setBusy(false)
    }
  }

  async function handleDelete(p: ProjectInfo) {
    if (!confirm(`确定删除项目「${p.name}」吗？删除后有 30 秒内可在右下角 Toast 撤销。`)) {
      return
    }
    setBusy(true)
    try {
      const res = await window.api.project.delete(p.id)
      if (!res.ok) {
        alert(`删除失败：${res.error}`)
        return
      }
      await reload()
      onChanged()
      if (res.trashPath && res.snapshot) {
        const trashPath = res.trashPath
        const snap = res.snapshot
        const timer = setTimeout(() => {
          void window.api.project.purgeTrash(trashPath)
        }, 30_000)
        showToast(`已删除项目「${snap.name}」`, {
          level: 'warn',
          duration: 30_000,
          action: {
            label: '撤销',
            onClick: async () => {
              clearTimeout(timer)
              const r = await window.api.project.undelete(trashPath, snap)
              if (r.ok) {
                showToast(`已恢复「${snap.name}」`, { level: 'success' })
                onChanged()
              } else {
                showToast(`撤销失败：${r.error ?? ''}`, { level: 'error' })
              }
            }
          }
        })
      }
    } finally {
      setBusy(false)
    }
  }

  async function handleRename(p: ProjectInfo) {
    const name = window.prompt('新名称：', p.name)
    if (!name?.trim() || name.trim() === p.name) return
    const res = await window.api.project.rename(p.id, name.trim())
    if (!res.ok) alert(`重命名失败：${res.error}`)
    else await reload()
  }

  async function handleChangeRepo(p: ProjectInfo) {
    const pick = await window.api.project.pickDir()
    if (pick.canceled || !pick.path) return
    const res = await window.api.project.setTargetRepo(p.id, pick.path)
    if (!res.ok) alert(`修改目标仓库失败：${res.error}`)
    else {
      await reload()
      onChanged()
    }
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal project-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>📁 项目管理</h3>
          <button
            className="drawer-btn primary"
            onClick={handleCreate}
            disabled={busy}
            style={{ marginLeft: 'auto', marginRight: 8 }}
          >
            ＋ 新建项目
          </button>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>
        <div className="project-list">
          {projects.length === 0 ? (
            <div className="drawer-empty">
              还没有项目，点击右上角「＋ 新建项目」创建第一个。
            </div>
          ) : (
            projects.map((p) => (
              <div
                key={p.id}
                className={`project-item ${currentId === p.id ? 'active' : ''}`}
              >
                <div className="project-main" onClick={() => onSelect(p)}>
                  <div className="project-name">
                    {p.name}
                    {currentId === p.id && <span className="project-current">（当前）</span>}
                  </div>
                  <div className="project-meta" title={p.target_repo}>
                    📂 {p.target_repo}
                  </div>
                  <div className="project-meta-sub">
                    更新: {new Date(p.updated_at).toLocaleString()}
                  </div>
                </div>
                <div className="project-actions">
                  <button className="tile-btn" onClick={() => handleRename(p)}>
                    ✎ 改名
                  </button>
                  <button className="tile-btn" onClick={() => handleChangeRepo(p)}>
                    📂 换仓库
                  </button>
                  <button className="tile-btn" onClick={() => handleDelete(p)}>
                    🗑 删除
                  </button>
                </div>
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  )
}
