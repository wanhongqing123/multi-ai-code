import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import ScheduledTaskDialog from './ScheduledTaskDialog'
import { formatDateTime } from './scheduledTaskViewModel'
import type { ScheduledTask } from '../../electron/preload'

function task(overrides: Partial<ScheduledTask> = {}): ScheduledTask {
  return {
    id: 1,
    projectId: 'project-1',
    targetRepo: null,
    name: 'Daily code review',
    description: 'Check project risks',
    goal: 'Check recent code changes in the current project.',
    instructions: ['Analyze code risk'],
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
        targetRepo={'E:\\OpenSource\\multi-ai-code'}
        sessionId="session-1"
        sessionRunning={true}
        initialTasks={[task()]}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('\u5b9a\u65f6\u4efb\u52a1\u7ba1\u7406')
    expect(markup).toContain('\u5f53\u524d AICLI\uff1a\u5df2\u542f\u52a8')
    expect(markup).toContain('+ \u65b0\u5efa\u4efb\u52a1')
    expect(markup).toContain('Daily code review')
    expect(markup).toContain('\u4efb\u52a1\u5185\u5bb9')
    expect(markup).toContain('AICLI \u7b56\u7565')
    expect(markup).toContain('\u5df2\u542f\u52a8\u5373\u53ef\u53d1\u9001')
    expect(markup).toContain('\u5173\u95ed')
    expect(markup).toContain('\u5220\u9664')
    expect(markup).not.toContain('<span>\u5df2\u542f\u7528\u4efb\u52a1</span><strong>')
    expect(markup).not.toContain('\u5f53\u524d AICLI\uff1a\u7a7a\u95f2')
  })

  it('renders an empty state without hiding the create action', () => {
    const markup = renderToStaticMarkup(
      <ScheduledTaskDialog
        projectId="project-1"
        targetRepo={'E:\\OpenSource\\multi-ai-code'}
        sessionId={null}
        sessionRunning={false}
        initialTasks={[]}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('\u8fd8\u6ca1\u6709\u5b9a\u65f6\u4efb\u52a1')
    expect(markup).toContain('+ \u65b0\u5efa\u4efb\u52a1')
    expect(markup).toContain('\u5f53\u524d AICLI\uff1a\u672a\u542f\u52a8')
  })

  it('does not render queue, running, or idle AICLI status cards', () => {
    const markup = renderToStaticMarkup(
      <ScheduledTaskDialog
        projectId="project-1"
        targetRepo={'E:\\OpenSource\\multi-ai-code'}
        sessionId="session-1"
        sessionRunning={true}
        initialTasks={[task()]}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('\u5f53\u524d AICLI\uff1a\u5df2\u542f\u52a8')
    expect(markup).not.toContain('<span>\u6392\u961f\u4e2d</span><strong>')
    expect(markup).not.toContain('<span>\u8fd0\u884c\u4e2d</span><strong>')
    expect(markup).not.toContain('<span>\u5f53\u524d AICLI</span><strong>')
    expect(markup).not.toContain('<span>\u5df2\u542f\u7528\u4efb\u52a1</span><strong>')
    expect(markup).not.toContain('\u5f53\u524d AICLI\uff1a\u7a7a\u95f2')
    expect(markup).not.toContain('\u5f53\u524d AICLI\uff1a\u6392\u961f\u4e2d')
  })

  it('does not derive the header state from queued items in the same repo', () => {
    const markup = renderToStaticMarkup(
      <ScheduledTaskDialog
        projectId="project-1"
        targetRepo={'E:\\OpenSource\\multi-ai-code'}
        sessionId="session-1"
        sessionRunning={true}
        initialTasks={[task()]}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('\u5f53\u524d AICLI\uff1a\u5df2\u542f\u52a8')
    expect(markup).not.toContain('<span>\u6392\u961f\u4e2d</span><strong>')
    expect(markup).not.toContain('<span>\u8fd0\u884c\u4e2d</span><strong>')
    expect(markup).not.toContain('\u5f53\u524d AICLI\uff1a\u7a7a\u95f2')
    expect(markup).not.toContain('\u5f53\u524d AICLI\uff1a\u6392\u961f\u4e2d')
  })

  it('renders task names and start times in the task list cards', () => {
    const hiddenTaskStartTime = Date.UTC(2026, 5, 22, 19, 30)
    const markup = renderToStaticMarkup(
      <ScheduledTaskDialog
        projectId="project-1"
        targetRepo={'E:\\OpenSource\\multi-ai-code'}
        sessionId="session-1"
        sessionRunning={true}
        initialTasks={[
          task({ id: 1, name: 'Selected task' }),
          task({
            id: 2,
            name: 'Hidden details task',
            description: 'Hidden card description with execution steps',
            goal: 'Hidden card goal should only appear after selecting the task',
            instructions: ['Hidden step one', 'Hidden step two'],
            nextRunAt: hiddenTaskStartTime
          })
        ]}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('Hidden details task')
    expect(markup).toContain(`开始时间：${formatDateTime(hiddenTaskStartTime)}`)
    expect(markup).not.toContain('Hidden card description with execution steps')
    expect(markup).not.toContain('Hidden card goal should only appear after selecting the task')
    expect(markup).not.toContain('Hidden step one')
  })
})
