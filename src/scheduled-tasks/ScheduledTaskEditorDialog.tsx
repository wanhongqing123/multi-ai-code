import type { CreateScheduledTaskInput, ScheduledTaskScheduleType } from '../../electron/preload'
import {
  DEFAULT_SCHEDULED_TASK_INSTRUCTIONS,
  buildScheduledTaskPreviewPrompt
} from './scheduledTaskViewModel'

interface Props {
  mode: 'create' | 'edit'
  draft: CreateScheduledTaskInput
  targetRepo: string
  onChange: (patch: Partial<CreateScheduledTaskInput>) => void
  onCancel: () => void
  onSave: () => void
}

const COMMON_INSTRUCTIONS = [
  '分析代码风险',
  '给出修改建议',
  '不要直接修改代码',
  '运行测试前先说明'
]

export default function ScheduledTaskEditorDialog(props: Props): JSX.Element {
  const { draft, mode, targetRepo, onCancel, onChange, onSave } = props
  const preview = buildScheduledTaskPreviewPrompt(draft, targetRepo)

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
    onChange({ scheduleType })
  }

  return (
    <div
      className="modal-backdrop"
      onClick={(event) => {
        event.stopPropagation()
        onCancel()
      }}
    >
      <div className="modal scheduled-task-editor-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-head">
          <h3>{mode === 'create' ? '＋ 新建定时任务' : '编辑定时任务'}</h3>
          <button className="modal-close" onClick={onCancel}>
            ×
          </button>
        </div>

        <div className="scheduled-task-editor">
          <aside className="scheduled-task-steps">
            <h4>配置步骤</h4>
            {['基本信息', 'AICLI 要做什么', '怎么干与限制', '执行时间', 'Prompt 预览'].map(
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
              <span>● AICLI 忙时排队等待</span>
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
            <label>
              <span>任务说明</span>
              <input
                value={draft.description}
                onChange={(event) => onChange({ description: event.target.value })}
                placeholder="每天检查当前项目最近代码风险，并给出建议。"
              />
            </label>
            <label>
              <span>让 AICLI 做什么</span>
              <textarea
                value={draft.goal}
                onChange={(event) => onChange({ goal: event.target.value })}
                placeholder="检查当前项目最近的代码变更，找出潜在风险。"
              />
            </label>

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

            <section>
              <span className="scheduled-task-field-title">执行方式</span>
              <div className="scheduled-task-policy-row">
                <span>● 使用当前 AICLI</span>
                <span>● 忙碌时排队等待</span>
              </div>
            </section>

            <div className="scheduled-task-form-row">
              <label>
                <span>频率</span>
                <select
                  value={draft.scheduleType}
                  onChange={(event) => setScheduleType(event.target.value as ScheduledTaskScheduleType)}
                >
                  <option value="once">一次性</option>
                  <option value="daily">每天</option>
                  <option value="weekly">每周</option>
                </select>
              </label>
              <label>
                <span>时间</span>
                <input
                  type="time"
                  value={draft.scheduleTime}
                  onChange={(event) => onChange({ scheduleTime: event.target.value })}
                />
              </label>
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

          <aside className="scheduled-task-preview">
            <h4>最终发送给 AICLI</h4>
            <p>保存前可确认真实 Prompt 内容</p>
            <pre>{preview}</pre>
            <div className="scheduled-task-preview-mini">
              <span>频率：{draft.scheduleType === 'daily' ? '每天' : draft.scheduleType === 'weekly' ? '每周' : '一次性'}</span>
              <span>时间：{draft.scheduleTime}</span>
            </div>
          </aside>
        </div>

        <div className="modal-actions">
          <button className="drawer-btn" onClick={onCancel}>
            取消
          </button>
          <button
            className="drawer-btn primary"
            onClick={onSave}
            disabled={!draft.name.trim() || !draft.goal.trim()}
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
