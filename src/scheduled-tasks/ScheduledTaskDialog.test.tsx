import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import ScheduledTaskDialog from './ScheduledTaskDialog'
import type { ScheduledTask } from '../../electron/preload'

function task(overrides: Partial<ScheduledTask> = {}): ScheduledTask {
  return {
    id: 1,
    projectId: 'project-1',
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
        targetRepo="E:\\OpenSource\\multi-ai-code"
        sessionId="session-1"
        sessionRunning={true}
        initialTasks={[task()]}
        initialQueueState={{ running: null, waiting: [] }}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('\u5b9a\u65f6\u4efb\u52a1\u7ba1\u7406')
    expect(markup).toContain('\u5f53\u524d AICLI\uff1a\u7a7a\u95f2')
    expect(markup).toContain('+ \u65b0\u5efa\u4efb\u52a1')
    expect(markup).toContain('Daily code review')
    expect(markup).toContain('\u4efb\u52a1\u5185\u5bb9')
    expect(markup).toContain('AICLI \u7b56\u7565')
    expect(markup).toContain('\u5fd9\u788c\u65f6\u6392\u961f\u7b49\u5f85')
    expect(markup).toContain('\u5173\u95ed')
    expect(markup).toContain('\u5220\u9664')
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

    expect(markup).toContain('\u8fd8\u6ca1\u6709\u5b9a\u65f6\u4efb\u52a1')
    expect(markup).toContain('+ \u65b0\u5efa\u4efb\u52a1')
    expect(markup).toContain('\u5f53\u524d AICLI\uff1a\u672a\u542f\u52a8')
  })

  it('ignores queued items from other projects when rendering the current AICLI state', () => {
    const markup = renderToStaticMarkup(
      <ScheduledTaskDialog
        projectId="project-1"
        targetRepo="E:\\OpenSource\\multi-ai-code"
        sessionId="session-1"
        sessionRunning={true}
        initialTasks={[task()]}
        initialQueueState={{
          running: null,
          waiting: [
            {
              taskId: 2,
              taskName: 'other project task',
              projectId: 'project-2',
              runId: 20,
              scheduledAt: Date.now(),
              prompt: 'run other project task'
            }
          ]
        }}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('\u6392\u961f\u4e2d</span><strong>0</strong>')
    expect(markup).toContain('\u5f53\u524d AICLI\uff1a\u7a7a\u95f2')
    expect(markup).not.toContain('\u5f53\u524d AICLI\uff1a\u6392\u961f\u4e2d')
  })
})
