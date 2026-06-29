import type { NormalTaskEntry } from './NormalTaskDialog'

export function buildNormalTaskRunPrompt(task: NormalTaskEntry, targetRepo: string): string {
  const description = task.description?.trim() || task.name
  const details = task.details?.trim() || '暂无任务详情'

  return [
    '你现在要执行一个由 Multi-AI Code 触发的普通任务。',
    '',
    `任务名称：${task.name}`,
    `工作目录：${targetRepo}`,
    `方案文档：${task.abs}`,
    '',
    '任务描述：',
    description,
    '',
    '任务详情：',
    details,
    '',
    '执行要求：',
    '1. 如果你正在处理上一项任务，不要中断当前工作；请在上一项任务完成后继续执行下面的普通任务。',
    '2. 如果当前没有正在处理的任务，请立即执行。',
    '3. 请根据任务描述和任务详情执行；必要时阅读或更新方案文档。',
    '4. 完成后用中文总结结果。'
  ].join('\n')
}
