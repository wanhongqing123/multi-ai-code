import { type FormEvent, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'

export interface NormalTaskEntry {
  name: string
  abs: string
  source: 'internal' | 'external'
  description?: string
  details?: string
}

export interface NormalTaskMetadataDraft {
  description: string
  details: string
}

function formatSaveError(err: unknown): string {
  const message = err instanceof Error ? err.message : String(err)
  return `保存任务信息失败：${message}`
}

interface Props {
  tasks: NormalTaskEntry[]
  selectedName: string
  sessionRunning: boolean
  onCreate: (
    name: string,
    description?: string,
    details?: string
  ) => Promise<NormalTaskEntry | null> | NormalTaskEntry | null
  onRun: (task: NormalTaskEntry) => Promise<void> | void
  onSelect?: (name: string) => void
  onPreview: (name: string) => void
  onSaveMetadata?: (name: string, metadata: NormalTaskMetadataDraft) => Promise<boolean> | boolean
  onSaveDescription?: (name: string, description: string) => Promise<boolean> | boolean
  onRefresh: () => Promise<void> | void
  onClose: () => void
}

export default function NormalTaskDialog(props: Props): JSX.Element {
  const {
    tasks,
    selectedName,
    sessionRunning,
    onCreate,
    onRun,
    onSelect,
    onPreview,
    onSaveMetadata,
    onSaveDescription,
    onRefresh,
    onClose
  } = props
  const [query, setQuery] = useState('')
  const [draftName, setDraftName] = useState('')
  const [createDescriptionDraft, setCreateDescriptionDraft] = useState('')
  const [createDetailsDraft, setCreateDetailsDraft] = useState('')
  const [creating, setCreating] = useState(false)
  const [creatingTask, setCreatingTask] = useState(false)
  const [createError, setCreateError] = useState<string | null>(null)
  const [localSelectedName, setLocalSelectedName] = useState(selectedName)

  const filteredTasks = useMemo(() => {
    const needle = query.trim().toLowerCase()
    if (!needle) return tasks
    return tasks.filter((task) =>
      [task.name, task.abs, task.source, task.description ?? '', task.details ?? '']
        .join(' ')
        .toLowerCase()
        .includes(needle)
    )
  }, [query, tasks])
  const selectedTask =
    tasks.find((task) => task.name === localSelectedName) ??
    tasks.find((task) => task.name === selectedName) ??
    filteredTasks[0] ??
    null
  const selectedDescription = selectedTask?.description?.trim() || selectedTask?.name || ''
  const selectedDetails = selectedTask?.details?.trim() ?? ''
  const [descriptionDraft, setDescriptionDraft] = useState(() => selectedDescription)
  const [detailsDraft, setDetailsDraft] = useState(() => selectedTask?.details ?? '')
  const [editingMetadata, setEditingMetadata] = useState(false)
  const [savingMetadata, setSavingMetadata] = useState(false)
  const [metadataSaveError, setMetadataSaveError] = useState<string | null>(null)
  const descriptionTextareaRef = useRef<HTMLTextAreaElement | null>(null)
  const detailsTextareaRef = useRef<HTMLTextAreaElement | null>(null)
  const aicliState = sessionRunning ? '已启动' : '未启动'

  const adjustMetadataTextareaHeight = useCallback(() => {
    for (const textarea of [descriptionTextareaRef.current, detailsTextareaRef.current]) {
      if (!textarea) continue
      textarea.style.height = 'auto'
      textarea.style.height = `${textarea.scrollHeight}px`
    }
  }, [])

  useEffect(() => {
    setDescriptionDraft(selectedDescription)
    setDetailsDraft(selectedTask?.details ?? '')
    setEditingMetadata(false)
    setMetadataSaveError(null)
  }, [selectedDescription, selectedTask?.details, selectedTask?.name])

  useEffect(() => {
    if (selectedName) setLocalSelectedName(selectedName)
  }, [selectedName])

  useEffect(() => {
    if (editingMetadata) adjustMetadataTextareaHeight()
  }, [adjustMetadataTextareaHeight, descriptionDraft, detailsDraft, editingMetadata])

  async function submitCreate(event: FormEvent<HTMLFormElement>): Promise<void> {
    event.preventDefault()
    const name = draftName.trim()
    if (!name || creating) return
    setCreating(true)
    setCreateError(null)
    try {
      const created = await onCreate(name, createDescriptionDraft, createDetailsDraft)
      if (created) {
        setDraftName('')
        setCreateDescriptionDraft('')
        setCreateDetailsDraft('')
        setQuery('')
        setCreatingTask(false)
        setLocalSelectedName(created.name)
        onSelect?.(created.name)
      } else {
        setCreateError('创建普通任务失败，请查看错误提示后重试。')
      }
    } finally {
      setCreating(false)
    }
  }

  async function saveTaskMetadata(
    name: string,
    metadata: NormalTaskMetadataDraft
  ): Promise<boolean> {
    if (onSaveMetadata) return await onSaveMetadata(name, metadata)
    if (onSaveDescription) return await onSaveDescription(name, metadata.description)
    return false
  }

  async function saveMetadata(): Promise<void> {
    if (!selectedTask || savingMetadata) return
    setSavingMetadata(true)
    setMetadataSaveError(null)
    try {
      const saved = await saveTaskMetadata(selectedTask.name, {
        description: descriptionDraft,
        details: detailsDraft
      })
      if (saved) {
        setEditingMetadata(false)
      } else {
        setMetadataSaveError('保存任务信息失败，请查看错误提示后重试。')
      }
    } catch (err: unknown) {
      setMetadataSaveError(formatSaveError(err))
    } finally {
      setSavingMetadata(false)
    }
  }

  function openCreateTask(): void {
    setDraftName('')
    setCreateDescriptionDraft('')
    setCreateDetailsDraft('')
    setCreateError(null)
    setMetadataSaveError(null)
    setEditingMetadata(false)
    setCreatingTask(true)
  }

  function startMetadataEdit(): void {
    if (!selectedTask) return
    setDescriptionDraft(selectedDescription)
    setDetailsDraft(selectedTask.details ?? '')
    setMetadataSaveError(null)
    setCreatingTask(false)
    setEditingMetadata(true)
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="modal scheduled-task-modal normal-task-modal"
        onClick={(event) => event.stopPropagation()}
      >
        <div className="modal-head">
          <h3>普通任务管理</h3>
          <span className={`scheduled-task-aicli ${aicliState === '已启动' ? 'idle' : ''}`}>
            当前 AICLI：{aicliState}
          </span>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="scheduled-task-body normal-task-body">
          <section className="scheduled-task-toolbar normal-task-toolbar">
            <button
              type="button"
              className="drawer-btn primary"
              onClick={openCreateTask}
              title="创建普通任务"
            >
              + 新建普通任务
            </button>
            <button type="button" className="drawer-btn" onClick={() => void onRefresh()}>
              刷新
            </button>
            <input
              className="scheduled-task-search"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              placeholder="搜索普通任务名称 / 路径..."
            />
          </section>

          <div className="scheduled-task-grid normal-task-grid">
            <section className="scheduled-task-list">
              <div className="scheduled-task-pane-head">
                <h4>普通任务列表</h4>
                <span>共 {tasks.length} 个</span>
              </div>
              {filteredTasks.length === 0 ? (
                <div className="scheduled-task-empty">
                  <strong>还没有普通任务</strong>
                  <span>点击“+ 新建普通任务”，创建当前项目内的任务文档。</span>
                </div>
              ) : (
                filteredTasks.map((task) => (
                  <div
                    className={`scheduled-task-card ${selectedTask?.name === task.name ? 'active' : ''}`}
                    key={`${task.source}:${task.name}`}
                  >
                    <button
                      type="button"
                      className="scheduled-task-card-select"
                      onClick={() => {
                        setCreatingTask(false)
                        setLocalSelectedName(task.name)
                        onSelect?.(task.name)
                      }}
                      title="选择普通任务"
                    >
                      <span className={`scheduled-task-dot ${task.source === 'external' ? 'warning' : 'success'}`} />
                      <span className="scheduled-task-card-main">
                        <strong>{task.name}</strong>
                        <small>{task.abs}</small>
                      </span>
                    </button>
                    <span className="scheduled-task-card-actions">
                      {task.source === 'external' && (
                        <span className="scheduled-task-card-action">外部方案</span>
                      )}
                      <button
                        type="button"
                        className="scheduled-task-card-action"
                        data-task-action="run"
                        title={sessionRunning ? '发送普通任务到当前 AICLI' : '请先启动 AICLI'}
                        onClick={() => void onRun(task)}
                      >
                        运行
                      </button>
                    </span>
                  </div>
                ))
              )}
            </section>

            <section className="scheduled-task-detail">
              {creatingTask ? (
                <form className="scheduled-task-form normal-task-create-detail" onSubmit={(event) => void submitCreate(event)}>
                  <h4>新建普通任务</h4>
                  <label>
                    <span>任务名称</span>
                    <input
                      value={draftName}
                      onChange={(event) => setDraftName(event.target.value)}
                      placeholder="修复登录流程"
                      disabled={creating}
                    />
                  </label>
                  <label>
                    <span>任务描述</span>
                    <textarea
                      className="normal-task-description-input scheduled-task-goal-input"
                      value={createDescriptionDraft}
                      onChange={(event) => setCreateDescriptionDraft(event.target.value)}
                      placeholder="一句话说明这个普通任务，留空时会使用任务名称。"
                      disabled={creating}
                    />
                  </label>
                  <label>
                    <span>任务详情</span>
                    <textarea
                      className="normal-task-description-input scheduled-task-goal-input"
                      value={createDetailsDraft}
                      onChange={(event) => setCreateDetailsDraft(event.target.value)}
                      placeholder="补充任务背景、目标、约束和执行说明，支持 Markdown。"
                      disabled={creating}
                    />
                  </label>
                  {createError && <p className="scheduled-task-last-run">{createError}</p>}
                  <div className="normal-task-description-actions">
                    <button
                      type="button"
                      className="drawer-btn"
                      disabled={creating}
                      onClick={() => {
                        setDraftName('')
                        setCreateDescriptionDraft('')
                        setCreateDetailsDraft('')
                        setCreateError(null)
                        setCreatingTask(false)
                      }}
                    >
                      取消
                    </button>
                    <button
                      type="submit"
                      className="drawer-btn primary"
                      disabled={!draftName.trim() || creating}
                    >
                      {creating ? '保存中...' : '保存任务'}
                    </button>
                  </div>
                </form>
              ) : selectedTask ? (
                <>
                  <div className="scheduled-task-detail-head">
                    <h4>{selectedTask.name}</h4>
                    <span>
                      <button
                        type="button"
                        className="drawer-btn"
                        onClick={startMetadataEdit}
                        disabled={editingMetadata}
                      >
                        编辑
                      </button>
                      <button
                        type="button"
                        className="drawer-btn"
                        onClick={() => onPreview(selectedTask.name)}
                      >
                        查看方案文档
                      </button>
                    </span>
                  </div>
                  {selectedTask.source === 'external' && (
                    <p className="scheduled-task-last-run">
                      外部方案：不再支持导入新的外部方案，已有映射仅保留用于兼容历史数据。
                    </p>
                  )}
                  {editingMetadata ? (
                    <div className="normal-task-description-editor">
                      <label className="normal-task-description-label">
                        <span>任务描述</span>
                        <textarea
                          ref={descriptionTextareaRef}
                          className="normal-task-description-input scheduled-task-goal-input"
                          value={descriptionDraft}
                          onInput={adjustMetadataTextareaHeight}
                          onChange={(event) => setDescriptionDraft(event.target.value)}
                          placeholder="一句话说明这个普通任务，留空时会使用任务名称。"
                        />
                      </label>
                      <label className="normal-task-description-label">
                        <span>任务详情</span>
                        <textarea
                          ref={detailsTextareaRef}
                          className="normal-task-description-input scheduled-task-goal-input normal-task-details-input"
                          value={detailsDraft}
                          onInput={adjustMetadataTextareaHeight}
                          onChange={(event) => setDetailsDraft(event.target.value)}
                          placeholder="补充任务背景、目标、约束和执行说明，支持 Markdown。"
                        />
                      </label>
                      <div className="normal-task-description-actions">
                        <button
                          type="button"
                          className="drawer-btn"
                          disabled={savingMetadata}
                          onClick={() => {
                            setDescriptionDraft(selectedDescription)
                            setDetailsDraft(selectedTask.details ?? '')
                            setEditingMetadata(false)
                          }}
                        >
                          取消
                        </button>
                        <button
                          type="button"
                          className="drawer-btn primary"
                          disabled={savingMetadata}
                          onClick={() => void saveMetadata()}
                        >
                          {savingMetadata ? '保存中...' : '保存任务信息'}
                        </button>
                      </div>
                      {metadataSaveError && (
                        <p className="scheduled-task-last-run">{metadataSaveError}</p>
                      )}
                    </div>
                  ) : (
                    <>
                      <h5>任务描述</h5>
                      <div className="normal-task-description-summary">
                        {selectedDescription}
                      </div>
                      <h5>任务详情</h5>
                      <div className="scheduled-task-markdown normal-task-details">
                        <ReactMarkdown remarkPlugins={[remarkGfm]}>
                          {selectedDetails || '暂无任务详情'}
                        </ReactMarkdown>
                      </div>
                    </>
                  )}
                  <h5>方案文档</h5>
                  <div className="scheduled-task-info-row normal-task-document-path">
                    <code title={selectedTask.abs}>{selectedTask.abs}</code>
                  </div>
                </>
              ) : (
                <div className="scheduled-task-empty">
                  <strong>选择一个普通任务查看详情</strong>
                </div>
              )}
            </section>
          </div>
        </div>
      </div>
    </div>
  )
}
