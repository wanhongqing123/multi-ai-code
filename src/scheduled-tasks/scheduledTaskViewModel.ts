import type {
  CreateScheduledTaskInput,
  ScheduledTaskRunStatus,
  ScheduledTaskScheduleType
} from '../../electron/preload'

export const DEFAULT_SCHEDULED_TASK_INSTRUCTIONS = [
  '分析代码风险',
  '给出修改建议',
  '不要直接修改代码'
]

export function createDefaultScheduledTaskDraft(projectId: string): CreateScheduledTaskInput {
  return {
    projectId,
    name: '',
    description: '',
    goal: '',
    instructions: [...DEFAULT_SCHEDULED_TASK_INSTRUCTIONS],
    enabled: true,
    scheduleType: 'daily',
    scheduleTime: '21:30',
    scheduleDays: [],
    timeoutMinutes: 30,
    allowCodeChanges: false,
    allowGitCommit: false,
    requireTestConfirmation: false
  }
}

export function buildScheduledTaskPreviewPrompt(
  draft: CreateScheduledTaskInput,
  targetRepo: string
): string {
  const requirementItems = draft.instructions.length
    ? [...draft.instructions]
    : ['按任务目标执行，并保持输出清晰。']
  requirementItems.push(
    `任务开始执行时先记录当前时间；任务完成时在总结中写明本次任务的执行时间范围和实际执行时长；任务时长上限：${draft.timeoutMinutes} 分钟。`
  )
  const requirements = requirementItems
    .map((instruction, index) => `${index + 1}. ${instruction}`)
    .join('\n')
  const safetyRules = [
    draft.allowCodeChanges
      ? '允许直接修改代码，但必须保持改动聚焦在本任务。'
      : '不要直接修改代码。',
    draft.allowGitCommit ? '允许按任务需要提交 git。' : '不要提交 git。',
    draft.requireTestConfirmation ? '如果需要运行测试，先说明要运行什么命令。' : null
  ].filter((line): line is string => Boolean(line))

  return [
    '你现在要执行一个由 Multi-AI Code 触发的定时任务。',
    '',
    `任务名称：${draft.name || '未命名定时任务'}`,
    `工作目录：${targetRepo || '当前项目'}`,
    `任务超时时间：${draft.timeoutMinutes} 分钟`,
    `如果无法在 ${draft.timeoutMinutes} 分钟内完成，请停止继续展开，输出当前进展、阻塞点和建议的下一步。`,
    '',
    '任务目标：',
    draft.goal || '请按用户配置的任务目标执行。',
    '',
    '执行要求：',
    requirements,
    '',
    '安全规则：',
    ...safetyRules.map((rule, index) => `${index + 1}. ${rule}`),
    '',
    '输出要求：',
    '1. 用中文总结。',
    '2. 明确列出发现的问题。',
    '3. 明确列出建议的下一步。'
  ].join('\n')
}

const WEEKDAY_LABELS = ['周日', '周一', '周二', '周三', '周四', '周五', '周六']

export function formatScheduleLabel(
  scheduleType: ScheduledTaskScheduleType,
  scheduleTime: string,
  scheduleDays: number[]
): string {
  if (scheduleType === 'once') return `一次性 ${scheduleTime}`
  if (scheduleType === 'daily') return `每天 ${scheduleTime}`
  const labels = scheduleDays
    .filter((day) => day >= 0 && day <= 6)
    .map((day) => WEEKDAY_LABELS[day])
  return `每${labels.length ? labels.join('、') : '周'} ${scheduleTime}`
}

export function formatDateTime(value: number | null): string {
  if (!value) return '未安排'
  return new Date(value).toLocaleString('zh-CN', {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit'
  })
}

export function formatScheduledTaskStatus(
  enabled: boolean,
  lastStatus: ScheduledTaskRunStatus | null
): { label: string; tone: 'success' | 'warning' | 'muted' | 'danger' | 'primary' } {
  if (!enabled) return { label: '禁用', tone: 'muted' }
  switch (lastStatus) {
    case 'queued':
      return { label: '排队', tone: 'warning' }
    case 'running':
      return { label: '运行中', tone: 'primary' }
    case 'succeeded':
      return { label: '成功', tone: 'success' }
    case 'failed':
    case 'timed_out':
      return { label: '失败', tone: 'danger' }
    case 'cancelled':
    case 'skipped':
      return { label: '跳过', tone: 'muted' }
    default:
      return { label: '等待中', tone: 'success' }
  }
}
