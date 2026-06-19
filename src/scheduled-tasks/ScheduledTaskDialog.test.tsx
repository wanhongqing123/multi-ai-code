import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import ScheduledTaskDialog from './ScheduledTaskDialog'
import type { ScheduledTask } from '../../electron/preload'

function task(overrides: Partial<ScheduledTask> = {}): ScheduledTask {
  return {
    id: 1,
    projectId: 'project-1',
    name: '每日代码巡检',
    description: '检查项目风险',
    goal: '检查当前项目最近的代码变更。',
    instructions: ['分析代码风险'],
    enabled: true,
    scheduleType: 'daily',
    scheduleTime: '21:30',
    scheduleDays: [],
    nextRunAt: Date.now(),
    timeoutMinutes: 30,
    allowCodeChanges: false,
    allowGitCommit: false,
    requireTestConfirmation: false,
    createdAt: 1,
    updatedAt: 1,
    lastRun: null,
    ...overrides
  }
}

describe('ScheduledTaskDialog', () => {
  it('renders the manager shell and selected task details', () => {
    const markup = renderToStaticMarkup(
      <ScheduledTaskDialog
        projectId="project-1"
        targetRepo="E:\\OpenSource\\multi-ai-code"
        sessionId="session-1"
        sessionRunning={true}
        initialTasks={[task()]}
        initialQueueState={{ running: null, waiting: [] }}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('定时任务管理')
    expect(markup).toContain('当前 AICLI：空闲')
    expect(markup).toContain('+ 新建任务')
    expect(markup).toContain('每日代码巡检')
    expect(markup).toContain('任务内容')
    expect(markup).toContain('AICLI 策略')
    expect(markup).toContain('忙碌时排队等待')
    expect(markup).toContain('关闭')
    expect(markup).toContain('删除')
  })

  it('renders an empty state without hiding the create action', () => {
    const markup = renderToStaticMarkup(
      <ScheduledTaskDialog
        projectId="project-1"
        targetRepo="E:\\OpenSource\\multi-ai-code"
        sessionId={null}
        sessionRunning={false}
        initialTasks={[]}
        initialQueueState={{ running: null, waiting: [] }}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('还没有定时任务')
    expect(markup).toContain('+ 新建任务')
    expect(markup).toContain('当前 AICLI：未启动')
  })
})
