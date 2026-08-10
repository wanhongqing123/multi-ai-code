import { ImageIcon } from '@primer/octicons-react'
import { useCallback, useEffect, useRef, useState } from 'react'
import type {
  CreateScheduledTaskInput,
  ScheduledTaskImageAttachment,
  ScheduledTaskScheduleType
} from '../../electron/preload'
import {
  DEFAULT_SCHEDULED_TASK_INSTRUCTIONS,
  parseScheduleIntervalMinutes
} from './scheduledTaskViewModel'

interface Props {
  mode: 'create' | 'edit'
  draft: CreateScheduledTaskInput
  targetRepo: string
  onChange: (patch: Partial<CreateScheduledTaskInput>) => void
  onAutoSave?: (draft: CreateScheduledTaskInput) => Promise<boolean> | boolean
  onCancel: () => void
  onSave: () => void
}

const SCHEDULED_TASK_GOAL_AUTOSAVE_DELAY_MS = 700
const MAX_SCHEDULED_TASK_IMAGES = 8

interface GoalInsertionPoint {
  goal: string
  start: number
  end: number
}

const COMMON_INSTRUCTIONS = [
  '分析代码风险',
  '给出修改建议',
  '不要直接修改代码',
  '运行测试前先说明'
]

function isClockScheduleTime(value: string): boolean {
  return /^\d{1,2}:\d{2}$/.test(value.trim())
}

function normalizeIntervalMinutes(value: string): number {
  return parseScheduleIntervalMinutes(value)
}

function descriptionFingerprint(draft: CreateScheduledTaskInput): string {
  return JSON.stringify({
    goal: draft.goal,
    imageAttachments: draft.imageAttachments
  })
}

function imageMarkdown(attachment: ScheduledTaskImageAttachment): string {
  const alt = attachment.fileName.replace(/\\/g, '\\\\').replace(/\]/g, '\\]')
  return `![${alt}](<${attachment.localPath}>)`
}

function hasImageMarkdown(goal: string, attachment: ScheduledTaskImageAttachment): boolean {
  return goal.includes(`(<${attachment.localPath}>)`)
}

export function insertScheduledTaskImageMarkdown(
  goal: string,
  attachments: ScheduledTaskImageAttachment[],
  start = goal.length,
  end = start
): string {
  if (attachments.length === 0) return goal
  const safeStart = Math.max(0, Math.min(start, goal.length))
  const safeEnd = Math.max(safeStart, Math.min(end, goal.length))
  const before = goal.slice(0, safeStart)
  const after = goal.slice(safeEnd)
  const markdown = attachments.map(imageMarkdown).join('\n')
  const prefix = before.length === 0 || before.endsWith('\n\n')
    ? ''
    : before.endsWith('\n')
      ? '\n'
      : '\n\n'
  const suffix = after.length === 0 || after.startsWith('\n\n')
    ? ''
    : after.startsWith('\n')
      ? '\n'
      : '\n\n'
  return `${before}${prefix}${markdown}${suffix}${after}`
}

export function appendMissingScheduledTaskImageMarkdown(
  goal: string,
  attachments: ScheduledTaskImageAttachment[]
): string {
  const missing = attachments.filter((attachment) => !hasImageMarkdown(goal, attachment))
  return insertScheduledTaskImageMarkdown(goal, missing)
}

export function referencedScheduledTaskImages(
  goal: string,
  attachments: ScheduledTaskImageAttachment[]
): ScheduledTaskImageAttachment[] {
  return attachments.filter((attachment) => hasImageMarkdown(goal, attachment))
}

export default function ScheduledTaskEditorDialog(props: Props): JSX.Element {
  const { draft, mode, onAutoSave, onCancel, onChange, onSave } = props
  const goalTextareaRef = useRef<HTMLTextAreaElement | null>(null)
  const imageInputRef = useRef<HTMLInputElement | null>(null)
  const goalAutosaveTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const latestDraftRef = useRef(draft)
  latestDraftRef.current = draft
  const editorGoal = appendMissingScheduledTaskImageMarkdown(draft.goal, draft.imageAttachments)
  const imageInsertionRef = useRef<GoalInsertionPoint>({
    goal: editorGoal,
    start: editorGoal.length,
    end: editorGoal.length
  })
  const currentDescriptionFingerprint = descriptionFingerprint(draft)
  const lastAutosavedDescriptionRef = useRef(currentDescriptionFingerprint)
  const [autosavingGoal, setAutosavingGoal] = useState(false)
  const [goalAutosaveError, setGoalAutosaveError] = useState<string | null>(null)
  const [imageUploadError, setImageUploadError] = useState<string | null>(null)
  const [uploadingImages, setUploadingImages] = useState(false)
  const [draggingImage, setDraggingImage] = useState(false)

  const adjustGoalTextareaHeight = useCallback(() => {
    const textarea = goalTextareaRef.current
    if (!textarea) return
    textarea.style.height = 'auto'
    textarea.style.height = `${textarea.scrollHeight}px`
  }, [])

  useEffect(() => {
    adjustGoalTextareaHeight()
  }, [adjustGoalTextareaHeight, editorGoal])

  useEffect(() => {
    if (editorGoal !== draft.goal) onChange({ goal: editorGoal })
  }, [draft.goal, editorGoal, onChange])

  const flushGoalAutosave = useCallback(async (): Promise<boolean> => {
    const fingerprint = descriptionFingerprint(draft)
    if (mode !== 'edit' || !onAutoSave || fingerprint === lastAutosavedDescriptionRef.current) {
      return true
    }
    setAutosavingGoal(true)
    setGoalAutosaveError(null)
    try {
      const saved = await onAutoSave(draft)
      if (saved) {
        lastAutosavedDescriptionRef.current = fingerprint
        return true
      }
      setGoalAutosaveError('任务描述自动保存失败，请稍后重试。')
      return false
    } catch (err: unknown) {
      const message = err instanceof Error ? err.message : String(err)
      setGoalAutosaveError(`任务描述自动保存失败：${message}`)
      return false
    } finally {
      setAutosavingGoal(false)
    }
  }, [draft, mode, onAutoSave])

  useEffect(() => {
    if (
      mode !== 'edit' ||
      !onAutoSave ||
      currentDescriptionFingerprint === lastAutosavedDescriptionRef.current
    ) {
      return
    }
    if (goalAutosaveTimerRef.current) {
      clearTimeout(goalAutosaveTimerRef.current)
    }
    goalAutosaveTimerRef.current = setTimeout(() => {
      void flushGoalAutosave()
    }, SCHEDULED_TASK_GOAL_AUTOSAVE_DELAY_MS)
    return () => {
      if (goalAutosaveTimerRef.current) {
        clearTimeout(goalAutosaveTimerRef.current)
        goalAutosaveTimerRef.current = null
      }
    }
  }, [currentDescriptionFingerprint, flushGoalAutosave, mode, onAutoSave])

  function rememberGoalSelection(textarea: HTMLTextAreaElement): GoalInsertionPoint {
    const insertion = {
      goal: textarea.value,
      start: textarea.selectionStart,
      end: textarea.selectionEnd
    }
    imageInsertionRef.current = insertion
    return insertion
  }

  async function addImageFiles(
    files: File[],
    insertion: GoalInsertionPoint = imageInsertionRef.current
  ): Promise<void> {
    const imageFiles = files.filter((file) => file.type.startsWith('image/'))
    if (imageFiles.length === 0) {
      setImageUploadError('请选择 PNG、JPEG、GIF 或 WebP 图片。')
      return
    }

    const remainingSlots = Math.max(0, MAX_SCHEDULED_TASK_IMAGES - draft.imageAttachments.length)
    if (remainingSlots === 0) {
      setImageUploadError(`每个任务最多添加 ${MAX_SCHEDULED_TASK_IMAGES} 张图片。`)
      return
    }

    const selectedFiles = imageFiles.slice(0, remainingSlots)
    setUploadingImages(true)
    setImageUploadError(null)
    const savedAttachments: ScheduledTaskImageAttachment[] = []
    const errors: string[] = []
    try {
      for (const file of selectedFiles) {
        const result = await window.api.scheduledTasks.saveImage({
          projectId: draft.projectId,
          fileName: file.name,
          mimeType: file.type,
          data: await file.arrayBuffer()
        })
        if (result.ok) {
          savedAttachments.push(result.attachment)
        } else {
          errors.push(`${file.name}：${result.error}`)
        }
      }
      if (savedAttachments.length > 0) {
        const currentDraft = latestDraftRef.current
        const currentGoal = appendMissingScheduledTaskImageMarkdown(
          currentDraft.goal,
          currentDraft.imageAttachments
        )
        const insertionStillMatches = insertion.goal === currentGoal
        const start = insertionStillMatches ? insertion.start : currentGoal.length
        const end = insertionStillMatches ? insertion.end : currentGoal.length
        const nextGoal = insertScheduledTaskImageMarkdown(
          currentGoal,
          savedAttachments,
          start,
          end
        )
        onChange({
          goal: nextGoal,
          imageAttachments: [...currentDraft.imageAttachments, ...savedAttachments]
        })
        imageInsertionRef.current = {
          goal: nextGoal,
          start: nextGoal.length,
          end: nextGoal.length
        }
      }
      if (imageFiles.length > remainingSlots) {
        errors.push(`每个任务最多添加 ${MAX_SCHEDULED_TASK_IMAGES} 张图片。`)
      }
      if (errors.length > 0) setImageUploadError(errors.join(' '))
    } catch (error) {
      setImageUploadError(error instanceof Error ? error.message : String(error))
    } finally {
      setUploadingImages(false)
      if (imageInputRef.current) imageInputRef.current.value = ''
    }
  }

  async function closeEditor(): Promise<void> {
    const saved = await flushGoalAutosave()
    if (saved) onCancel()
  }

  function toggleInstruction(instruction: string): void {
    const exists = draft.instructions.includes(instruction)
    const nextInstructions = exists
      ? draft.instructions.filter((item) => item !== instruction)
      : [...draft.instructions, instruction]
    onChange({
      instructions: nextInstructions,
      ...(instruction === '运行测试前先说明'
        ? { requireTestConfirmation: !draft.requireTestConfirmation }
        : {})
    })
  }

  function setAllowCodeChanges(allowCodeChanges: boolean): void {
    onChange({
      allowCodeChanges,
      instructions: allowCodeChanges
        ? draft.instructions.filter((instruction) => instruction !== '不要直接修改代码')
        : Array.from(new Set([...draft.instructions, '不要直接修改代码']))
    })
  }

  function setScheduleType(scheduleType: ScheduledTaskScheduleType): void {
    const patch: Partial<CreateScheduledTaskInput> = { scheduleType }
    if (scheduleType === 'interval') {
      patch.scheduleTime = String(normalizeIntervalMinutes(draft.scheduleTime))
      patch.scheduleDays = []
    } else {
      if (!isClockScheduleTime(draft.scheduleTime)) {
        patch.scheduleTime = '21:30'
      }
      if (scheduleType !== 'weekly') {
        patch.scheduleDays = []
      }
    }
    onChange(patch)
  }

  function setIntervalMinutes(value: string): void {
    onChange({ scheduleTime: String(normalizeIntervalMinutes(value)) })
  }

  return (
    <div
      className="modal-backdrop"
      onClick={(event) => {
        event.stopPropagation()
        void closeEditor()
      }}
    >
      <div className="modal scheduled-task-editor-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-head">
          <h3>{mode === 'create' ? '＋ 新建任务' : '编辑任务'}</h3>
          <button className="modal-close" onClick={() => void closeEditor()}>
            ×
          </button>
        </div>

        <div className="scheduled-task-editor">
          <aside className="scheduled-task-steps">
            <h4>配置步骤</h4>
            {['基本信息', '任务描述', '怎么干与限制', '执行时间'].map(
              (label, index) => (
                <div className={`scheduled-task-step ${index < 2 ? 'active' : ''}`} key={label}>
                  <span>{index + 1}</span>
                  <strong>{label}</strong>
                </div>
              )
            )}
            <section className="scheduled-task-safety">
              <strong>安全策略</strong>
              <span>● 默认不允许自动改代码</span>
              <span>● 默认不允许自动提交</span>
            </section>
          </aside>

          <main className="scheduled-task-form">
            <h4>任务配置</h4>
            <label>
              <span>任务名称</span>
              <input
                value={draft.name}
                onChange={(event) => onChange({ name: event.target.value })}
                placeholder="每日代码巡检"
              />
            </label>
            <div className="scheduled-task-goal-field">
              <span>任务描述</span>
              <div
                className={`scheduled-task-goal-shell ${draggingImage ? 'dragging' : ''}`}
                onDragOver={(event) => {
                  event.preventDefault()
                  setDraggingImage(true)
                }}
                onDragLeave={(event) => {
                  if (!event.currentTarget.contains(event.relatedTarget as Node | null)) {
                    setDraggingImage(false)
                  }
                }}
                onDrop={(event) => {
                  event.preventDefault()
                  setDraggingImage(false)
                  void addImageFiles(Array.from(event.dataTransfer.files), imageInsertionRef.current)
                }}
              >
                <textarea
                  ref={goalTextareaRef}
                  className="scheduled-task-goal-input"
                  value={editorGoal}
                  onInput={adjustGoalTextareaHeight}
                  onSelect={(event) => rememberGoalSelection(event.currentTarget)}
                  onChange={(event) => {
                    const goal = event.target.value
                    imageInsertionRef.current = {
                      goal,
                      start: event.target.selectionStart,
                      end: event.target.selectionEnd
                    }
                    onChange({
                      goal,
                      imageAttachments: referencedScheduledTaskImages(
                        goal,
                        draft.imageAttachments
                      )
                    })
                  }}
                  onPaste={(event) => {
                    const files = Array.from(event.clipboardData.items)
                      .filter((item) => item.kind === 'file' && item.type.startsWith('image/'))
                      .flatMap((item) => {
                        const file = item.getAsFile()
                        return file ? [file] : []
                      })
                    if (files.length === 0) return
                    event.preventDefault()
                    void addImageFiles(files, rememberGoalSelection(event.currentTarget))
                  }}
                  placeholder="检查当前项目最近的代码变更，找出潜在风险。"
                />
                <button
                  type="button"
                  className="scheduled-task-add-image"
                  disabled={uploadingImages}
                  title="添加图片"
                  aria-label="添加图片"
                  onMouseDown={() => {
                    if (goalTextareaRef.current) rememberGoalSelection(goalTextareaRef.current)
                  }}
                  onClick={() => imageInputRef.current?.click()}
                >
                  <ImageIcon size={18} />
                </button>
                <input
                  ref={imageInputRef}
                  className="scheduled-task-image-input"
                  type="file"
                  accept="image/png,image/jpeg,image/gif,image/webp"
                  multiple
                  onChange={(event) => void addImageFiles(Array.from(event.target.files ?? []))}
                />
              </div>
              {uploadingImages && (
                <small className="scheduled-task-last-run">图片保存中...</small>
              )}
              {imageUploadError && (
                <small className="scheduled-task-last-run">{imageUploadError}</small>
              )}
              {mode === 'edit' && autosavingGoal && (
                <small className="scheduled-task-last-run">任务描述与图片自动保存中...</small>
              )}
              {goalAutosaveError && (
                <small className="scheduled-task-last-run">{goalAutosaveError}</small>
              )}
            </div>

            <section>
              <span className="scheduled-task-field-title">具体要求</span>
              <div className="scheduled-task-check-grid">
                {COMMON_INSTRUCTIONS.map((instruction) => (
                  <label className="scheduled-task-check" key={instruction}>
                    <input
                      type="checkbox"
                      checked={draft.instructions.includes(instruction)}
                      onChange={() => toggleInstruction(instruction)}
                    />
                    <span>{instruction}</span>
                  </label>
                ))}
              </div>
            </section>

            <section>
              <span className="scheduled-task-field-title">安全限制</span>
              <div className="scheduled-task-check-grid">
                <label className="scheduled-task-check">
                  <input
                    type="checkbox"
                    checked={draft.allowCodeChanges}
                    onChange={(event) => setAllowCodeChanges(event.target.checked)}
                  />
                  <span>允许直接修改代码</span>
                </label>
                <label className="scheduled-task-check">
                  <input
                    type="checkbox"
                    checked={draft.allowGitCommit}
                    onChange={(event) => onChange({ allowGitCommit: event.target.checked })}
                  />
                  <span>允许提交 git</span>
                </label>
                <label className="scheduled-task-check">
                  <input
                    type="checkbox"
                    checked={draft.requireTestConfirmation}
                    onChange={(event) => onChange({ requireTestConfirmation: event.target.checked })}
                  />
                  <span>运行测试前先说明</span>
                </label>
              </div>
            </section>

            {draft.scheduleType === 'weekly' && (
              <section>
                <span className="scheduled-task-field-title">每周执行日</span>
                <div className="scheduled-task-weekdays">
                  {[
                    ['周日', 0],
                    ['周一', 1],
                    ['周二', 2],
                    ['周三', 3],
                    ['周四', 4],
                    ['周五', 5],
                    ['周六', 6]
                  ].map(([label, day]) => {
                    const dayNumber = Number(day)
                    return (
                      <label key={dayNumber}>
                        <input
                          type="checkbox"
                          checked={draft.scheduleDays.includes(dayNumber)}
                          onChange={(event) =>
                            onChange({
                              scheduleDays: event.target.checked
                                ? [...draft.scheduleDays, dayNumber].sort()
                                : draft.scheduleDays.filter((item) => item !== dayNumber)
                            })
                          }
                        />
                        <span>{label}</span>
                      </label>
                    )
                  })}
                </div>
              </section>
            )}

            <div className="scheduled-task-form-row">
              <label>
                <span>触发方式</span>
                <select
                  value={draft.scheduleType}
                  onChange={(event) => setScheduleType(event.target.value as ScheduledTaskScheduleType)}
                >
                  <option value="manual">手动执行</option>
                  <option value="once">一次性</option>
                  <option value="daily">每天</option>
                  <option value="weekly">每周</option>
                  <option value="interval">每隔</option>
                </select>
              </label>
              {/* 手动任务没有时间可填：它只在用户点「立即执行」时跑。放一个禁用的
                  时间框比整块消失更稳——行内控件数量不变，布局不会跳。 */}
              {draft.scheduleType === 'manual' ? (
                <label className="scheduled-task-manual-hint">
                  <span>执行时机</span>
                  <span className="scheduled-task-manual-note">在列表里点「▶ 立即执行」</span>
                </label>
              ) : draft.scheduleType === 'interval' ? (
                <label>
                  <span>间隔分钟</span>
                  <input
                    type="number"
                    min={1}
                    step={1}
                    value={String(parseScheduleIntervalMinutes(draft.scheduleTime))}
                    onChange={(event) => setIntervalMinutes(event.target.value)}
                  />
                </label>
              ) : (
                <label>
                  <span>时间</span>
                  <input
                    type="time"
                    value={draft.scheduleTime}
                    onChange={(event) => onChange({ scheduleTime: event.target.value })}
                  />
                </label>
              )}
              <label>
                <span>超时时间</span>
                <input
                  value={String(draft.timeoutMinutes)}
                  onChange={(event) =>
                    onChange({ timeoutMinutes: Math.max(1, Number(event.target.value) || 30) })
                  }
                />
              </label>
            </div>
          </main>

        </div>

        <div className="modal-actions">
          <button className="drawer-btn" onClick={() => void closeEditor()}>
            {mode === 'edit' ? '关闭' : '取消'}
          </button>
          <button
            className="drawer-btn primary"
            onClick={onSave}
            disabled={
              uploadingImages ||
              !draft.name.trim() ||
              (!draft.goal.trim() && draft.imageAttachments.length === 0)
            }
          >
            保存任务
          </button>
        </div>
      </div>
    </div>
  )
}

export function ensureDefaultInstructions(instructions: string[]): string[] {
  return instructions.length ? instructions : [...DEFAULT_SCHEDULED_TASK_INSTRUCTIONS]
}
