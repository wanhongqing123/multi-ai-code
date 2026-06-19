import type { ScheduledTask } from './types.js'

export interface ScheduledTaskPromptContext {
  targetRepo: string
}

export function buildScheduledTaskPrompt(
  task: ScheduledTask,
  context: ScheduledTaskPromptContext
): string {
  const requirements = task.instructions.length
    ? task.instructions.map((instruction, index) => `${index + 1}. ${instruction}`).join('\n')
    : '1. 按任务目标执行，并保持输出清晰。'
  const safetyRules = [
    task.allowCodeChanges
      ? '允许直接修改代码，但必须保持改动聚焦在本任务。'
      : '不要直接修改代码。',
    task.allowGitCommit ? '允许按任务需要提交 git。' : '不要提交 git。',
    task.requireTestConfirmation ? '如果需要运行测试，先说明要运行什么命令。' : null
  ].filter((line): line is string => Boolean(line))

  return [
    '你现在要执行一个由 Multi-AI Code 触发的定时任务。',
    '',
    `任务名称：${task.name}`,
    `工作目录：${context.targetRepo}`,
    '',
    '任务目标：',
    task.goal,
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
