import { type FormEvent, useMemo, useState } from 'react'

export interface NormalTaskEntry {
  name: string
  abs: string
  source: 'internal' | 'external'
}

interface Props {
  tasks: NormalTaskEntry[]
  selectedName: string
  sessionRunning: boolean
  onCreate: (name: string) => Promise<void> | void
  onSelect: (name: string) => void
  onPreview: (name: string) => void
  onRefresh: () => Promise<void> | void
  onClose: () => void
}

export default function NormalTaskDialog(props: Props): JSX.Element {
  const {
    tasks,
    selectedName,
    sessionRunning,
    onCreate,
    onSelect,
    onPreview,
    onRefresh,
    onClose
  } = props
  const [query, setQuery] = useState('')
  const [draftName, setDraftName] = useState('')
  const [creating, setCreating] = useState(false)

  const filteredTasks = useMemo(() => {
    const needle = query.trim().toLowerCase()
    if (!needle) return tasks
    return tasks.filter((task) =>
      [task.name, task.abs, task.source].join(' ').toLowerCase().includes(needle)
    )
  }, [query, tasks])
  const selectedTask =
    tasks.find((task) => task.name === selectedName) ?? filteredTasks[0] ?? null
  const aicliState = sessionRunning ? '已启动' : '未启动'

  async function submitCreate(event: FormEvent<HTMLFormElement>): Promise<void> {
    event.preventDefault()
    const name = draftName.trim()
    if (!name || creating) return
    setCreating(true)
    try {
      await onCreate(name)
      setDraftName('')
    } finally {
      setCreating(false)
    }
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
            <form className="normal-task-create-form" onSubmit={(event) => void submitCreate(event)}>
              <input
                className="scheduled-task-search normal-task-name-input"
                value={draftName}
                onChange={(event) => setDraftName(event.target.value)}
                placeholder="输入普通任务名"
                disabled={creating || sessionRunning}
              />
              <button
                type="submit"
                className="drawer-btn primary"
                disabled={!draftName.trim() || creating || sessionRunning}
                title={sessionRunning ? '运行中无法新建普通任务，请先停止当前 AICLI' : '创建普通任务'}
              >
                + 新建普通任务
              </button>
            </form>
            <button className="drawer-btn" onClick={() => void onRefresh()}>
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
                      onClick={() => onSelect(task.name)}
                      disabled={sessionRunning && selectedName !== task.name}
                      title={
                        sessionRunning && selectedName !== task.name
                          ? '运行中无法切换普通任务'
                          : '选择普通任务'
                      }
                    >
                      <span className={`scheduled-task-dot ${task.source === 'external' ? 'warning' : 'success'}`} />
                      <span className="scheduled-task-card-main">
                        <strong>{task.name}</strong>
                        <small>{task.abs}</small>
                      </span>
                    </button>
                    {task.source === 'external' && (
                      <span className="scheduled-task-card-action">外部方案</span>
                    )}
                  </div>
                ))
              )}
            </section>

            <section className="scheduled-task-detail">
              {selectedTask ? (
                <>
                  <div className="scheduled-task-detail-head">
                    <h4>{selectedTask.name}</h4>
                    <span>
                      <button
                        className="drawer-btn"
                        onClick={() => onSelect(selectedTask.name)}
                        disabled={sessionRunning && selectedName !== selectedTask.name}
                      >
                        使用
                      </button>
                      <button className="drawer-btn" onClick={() => onPreview(selectedTask.name)}>
                        查看任务文档
                      </button>
                    </span>
                  </div>
                  {selectedTask.source === 'external' && (
                    <p className="scheduled-task-last-run">
                      外部方案：不再支持导入新的外部方案，已有映射仅保留用于兼容历史数据。
                    </p>
                  )}
                  <h5>任务文档</h5>
                  <div className="scheduled-task-info-row">
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
