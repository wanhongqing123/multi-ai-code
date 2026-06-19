import { useEffect, useMemo, useState } from 'react'
import type {
  CreateScheduledTaskInput,
  ScheduledTask,
  ScheduledTaskQueueState
} from '../../electron/preload'
import ScheduledTaskEditorDialog, { ensureDefaultInstructions } from './ScheduledTaskEditorDialog'
import {
  createDefaultScheduledTaskDraft,
  formatDateTime,
  formatScheduleLabel,
  formatScheduledTaskStatus
} from './scheduledTaskViewModel'

interface Props {
  projectId: string
  targetRepo: string
  sessionId: string | null
  sessionRunning: boolean
  initialTasks?: ScheduledTask[]
  initialQueueState?: ScheduledTaskQueueState
  onClose: () => void
}

type EditorState =
  | { mode: 'create'; draft: CreateScheduledTaskInput; taskId?: undefined }
  | { mode: 'edit'; draft: CreateScheduledTaskInput; taskId: number }

const EMPTY_QUEUE: ScheduledTaskQueueState = { running: null, waiting: [] }

function taskToDraft(task: ScheduledTask): CreateScheduledTaskInput {
  return {
    projectId: task.projectId,
    name: task.name,
    description: task.description,
    goal: task.goal,
    instructions: ensureDefaultInstructions(task.instructions),
    enabled: task.enabled,
    scheduleType: task.scheduleType,
    scheduleTime: task.scheduleTime,
    scheduleDays: task.scheduleDays,
    timeoutMinutes: task.timeoutMinutes,
    allowCodeChanges: task.allowCodeChanges,
    allowGitCommit: task.allowGitCommit,
    requireTestConfirmation: task.requireTestConfirmation
  }
}

export default function ScheduledTaskDialog(props: Props): JSX.Element {
  const {
    projectId,
    targetRepo,
    sessionId,
    sessionRunning,
    initialTasks,
    initialQueueState,
    onClose
  } = props
  const [tasks, setTasks] = useState<ScheduledTask[]>(initialTasks ?? [])
  const [queueState, setQueueState] = useState<ScheduledTaskQueueState>(
    initialQueueState ?? EMPTY_QUEUE
  )
  const [selectedId, setSelectedId] = useState<number | null>(initialTasks?.[0]?.id ?? null)
  const [query, setQuery] = useState('')
  const [editor, setEditor] = useState<EditorState | null>(null)

  async function refresh(): Promise<void> {
    if (typeof window === 'undefined' || !window.api?.scheduledTasks) return
    const [nextTasks, nextQueue] = await Promise.all([
      window.api.scheduledTasks.list(projectId),
      window.api.scheduledTasks.queueState()
    ])
    setTasks(nextTasks)
    setQueueState(nextQueue)
    setSelectedId((current) => current ?? nextTasks[0]?.id ?? null)
  }

  useEffect(() => {
    if (initialTasks) return
    void refresh()
  }, [projectId])

  const filteredTasks = useMemo(() => {
    const needle = query.trim().toLowerCase()
    if (!needle) return tasks
    return tasks.filter((task) =>
      [task.name, task.description, task.goal, task.instructions.join(' ')]
        .join(' ')
        .toLowerCase()
        .includes(needle)
    )
  }, [query, tasks])

  const selectedTask = tasks.find((task) => task.id === selectedId) ?? filteredTasks[0] ?? null
  const enabledCount = tasks.filter((task) => task.enabled).length
  const runningCount = queueState.running ? 1 : 0
  const aicliState = !sessionRunning
    ? '未启动'
    : runningCount > 0
      ? '运行中'
      : queueState.waiting.length > 0
        ? '排队中'
        : '空闲'

  async function saveEditor(): Promise<void> {
    if (!editor || typeof window === 'undefined' || !window.api?.scheduledTasks) return
    if (editor.mode === 'create') {
      const result = await window.api.scheduledTasks.create(editor.draft)
      if (result.ok) {
        setEditor(null)
        await refresh()
      }
      return
    }
    const result = await window.api.scheduledTasks.update(editor.taskId, editor.draft)
    if (result.ok) {
      setEditor(null)
      await refresh()
    }
  }

  async function setAllEnabled(enabled: boolean): Promise<void> {
    if (typeof window === 'undefined' || !window.api?.scheduledTasks) return
    await Promise.all(tasks.map((task) => window.api.scheduledTasks.setEnabled(task.id, enabled)))
    await refresh()
  }

  async function setTaskEnabled(task: ScheduledTask, enabled: boolean): Promise<void> {
    if (typeof window === 'undefined' || !window.api?.scheduledTasks) return
    await window.api.scheduledTasks.setEnabled(task.id, enabled)
    await refresh()
  }

  async function deleteTask(task: ScheduledTask): Promise<void> {
    if (typeof window === 'undefined' || !window.api?.scheduledTasks) return
    const confirmed =
      typeof window.confirm !== 'function' ||
      window.confirm(`确认删除定时任务「${task.name}」吗？`)
    if (!confirmed) return
    await window.api.scheduledTasks.delete(task.id)
    setSelectedId(null)
    await refresh()
  }

  async function runNow(task: ScheduledTask): Promise<void> {
    if (typeof window === 'undefined' || !window.api?.scheduledTasks) return
    await window.api.scheduledTasks.runNow({ taskId: task.id, sessionId, targetRepo })
    await refresh()
  }

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal scheduled-task-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-head">
          <h3>⏰ 定时任务管理</h3>
          <span className={`scheduled-task-aicli ${aicliState === '空闲' ? 'idle' : ''}`}>
            当前 AICLI：{aicliState}
          </span>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="scheduled-task-body">
          <section className="scheduled-task-summary">
            <div>
              <span>已启用任务</span>
              <strong>{enabledCount}</strong>
            </div>
            <div>
              <span>排队中</span>
              <strong>{queueState.waiting.length}</strong>
            </div>
            <div>
              <span>运行中</span>
              <strong>{runningCount}</strong>
            </div>
            <div>
              <span>当前 AICLI</span>
              <strong>{aicliState === '空闲' ? '空闲 · 可执行定时任务' : aicliState}</strong>
            </div>
          </section>

          <section className="scheduled-task-toolbar">
            <button
              className="drawer-btn primary"
              onClick={() =>
                setEditor({
                  mode: 'create',
                  draft: createDefaultScheduledTaskDraft(projectId)
                })
              }
            >
              + 新建任务
            </button>
            <button className="drawer-btn" onClick={() => void setAllEnabled(true)}>
              全部启用
            </button>
            <button className="drawer-btn" onClick={() => void setAllEnabled(false)}>
              全部禁用
            </button>
            <button className="drawer-btn" onClick={() => void refresh()}>
              刷新
            </button>
            <input
              className="scheduled-task-search"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              placeholder="搜索任务名称 / prompt / 状态..."
            />
          </section>

          <div className="scheduled-task-grid">
            <section className="scheduled-task-list">
              <div className="scheduled-task-pane-head">
                <h4>任务列表</h4>
                <span>共 {tasks.length} 个</span>
              </div>
              {filteredTasks.length === 0 ? (
                <div className="scheduled-task-empty">
                  <strong>还没有定时任务</strong>
                  <span>点击“+ 新建任务”，告诉 AICLI 到点后要做什么。</span>
                </div>
              ) : (
                filteredTasks.map((task) => {
                  const status = formatScheduledTaskStatus(task.enabled, task.lastRun?.status ?? null)
                  return (
                    <button
                      type="button"
                      className={`scheduled-task-card ${selectedTask?.id === task.id ? 'active' : ''}`}
                      key={task.id}
                      onClick={() => setSelectedId(task.id)}
                    >
                      <span className={`scheduled-task-dot ${status.tone}`} />
                      <span className="scheduled-task-card-main">
                        <strong>{task.name}</strong>
                        <small>{task.description || task.goal}</small>
                        <em>
                          下次执行：{formatDateTime(task.nextRunAt)} · 最近结果：
                          {task.lastRun?.status ? status.label : '未运行'}
                        </em>
                      </span>
                      <span className={`scheduled-task-state ${status.tone}`}>{status.label}</span>
                    </button>
                  )
                })
              )}
            </section>

            <section className="scheduled-task-detail">
              {selectedTask ? (
                <>
                  <div className="scheduled-task-detail-head">
                    <h4>{selectedTask.name}</h4>
                    <span>
                      <button className="drawer-btn" onClick={() => void runNow(selectedTask)}>
                        运行
                      </button>
                      <button
                        className="drawer-btn"
                        onClick={() => void setTaskEnabled(selectedTask, !selectedTask.enabled)}
                      >
                        {selectedTask.enabled ? '关闭' : '启用'}
                      </button>
                      <button
                        className="drawer-btn"
                        onClick={() =>
                          setEditor({
                            mode: 'edit',
                            taskId: selectedTask.id,
                            draft: taskToDraft(selectedTask)
                          })
                        }
                      >
                        编辑
                      </button>
                      <button className="drawer-btn danger" onClick={() => void deleteTask(selectedTask)}>
                        删除
                      </button>
                    </span>
                  </div>
                  <h5>任务内容</h5>
                  <p className="scheduled-task-goal">{selectedTask.goal}</p>
                  <h5>执行计划</h5>
                  <div className="scheduled-task-info-row">
                    <span>频率：{formatScheduleLabel(selectedTask.scheduleType, selectedTask.scheduleTime, selectedTask.scheduleDays)}</span>
                    <span>超时：{selectedTask.timeoutMinutes} 分钟</span>
                  </div>
                  <h5>AICLI 策略</h5>
                  <div className="scheduled-task-info-row">
                    <span>使用当前 AICLI</span>
                    <span>忙碌时排队等待</span>
                  </div>
                  <h5>最近运行记录</h5>
                  <p className="scheduled-task-last-run">
                    {selectedTask.lastRun
                      ? `${formatDateTime(selectedTask.lastRun.finishedAt ?? selectedTask.lastRun.startedAt ?? selectedTask.lastRun.scheduledAt)} · ${formatScheduledTaskStatus(selectedTask.enabled, selectedTask.lastRun.status).label}`
                      : '还没有运行记录'}
                  </p>
                </>
              ) : (
                <div className="scheduled-task-empty">
                  <strong>选择一个任务查看详情</strong>
                </div>
              )}
            </section>
          </div>
        </div>
      </div>

      {editor && (
        <ScheduledTaskEditorDialog
          mode={editor.mode}
          draft={editor.draft}
          targetRepo={targetRepo}
          onChange={(patch) =>
            setEditor((current) => (current ? { ...current, draft: { ...current.draft, ...patch } } : current))
          }
          onCancel={() => setEditor(null)}
          onSave={() => void saveEditor()}
        />
      )}
    </div>
  )
}
