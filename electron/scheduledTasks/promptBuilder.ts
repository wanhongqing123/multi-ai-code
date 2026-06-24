import type { ScheduledTask } from './types.js'

function buildTimingRequirement(task: ScheduledTask): string {
  return `任务开始执行时先记录当前时间；任务完成时在总结中写明本次任务的执行时间范围和实际执行时长；任务时长上限：${task.timeoutMinutes} 分钟。`
}

export interface ScheduledTaskPromptContext {
  targetRepo: string
}

export function buildScheduledTaskPrompt(
  task: ScheduledTask,
  context: ScheduledTaskPromptContext
): string {
  const requirementItems = task.instructions.length
    ? [...task.instructions]
    : ['按任务描述执行，并保持输出清晰。']
  requirementItems.push(buildTimingRequirement(task))
  const requirements = requirementItems
    .map((instruction, index) => `${index + 1}. ${instruction}`)
    .join('\n')
  const safetyRules = [
    task.allowCodeChanges
      ? '允许直接修改代码，但必须保持改动聚焦在本任务。'
      : '不要直接修改代码。',
    task.allowGitCommit ? '允许按任务需要提交 git。' : '不要提交 git。',
    task.requireTestConfirmation ? '如果需要运行测试，先说明要运行什么命令。' : null
  ].filter((line): line is string => Boolean(line))

  return [
    '你现在要执行一个由 Multi-AI Code 触发的定时任务。',
    '如果你正在处理上一项任务，不要中断当前工作；请在上一项任务完成后继续执行下面的定时任务。如果当前没有正在处理的任务，请立即执行。',
    '',
    `任务名称：${task.name}`,
    `工作目录：${context.targetRepo}`,
    `任务超时时间：${task.timeoutMinutes} 分钟`,
    `如果无法在 ${task.timeoutMinutes} 分钟内完成，请停止继续展开，输出当前进展、阻塞点和建议的下一步。`,
    '',
    '\u4efb\u52a1\u63cf\u8ff0\uff1a',
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
