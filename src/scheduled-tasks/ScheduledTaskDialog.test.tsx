import { renderToStaticMarkup } from 'react-dom/server'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import ScheduledTaskDialog from './ScheduledTaskDialog'
import { formatDateTime } from './scheduledTaskViewModel'
import type { ScheduledTask } from '../../electron/preload'

function normalizeNewlines(value: string): string {
  return value.replace(/\r\n/g, '\n')
}

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

  it('uses a larger manager modal close to the editor size', () => {
    const styles = normalizeNewlines(
      readFileSync(fileURLToPath(new URL('../styles.css', import.meta.url)), 'utf8')
    )

    expect(styles).toContain('.scheduled-task-modal {\n  width: min(1380px, calc(100vw - 40px));')
    expect(styles).toContain('height: min(900px, calc(100vh - 32px));')
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
    expect(markup).toContain(`下次执行：${formatDateTime(hiddenTaskStartTime)}`)
    expect(markup).not.toContain('Hidden card description with execution steps')
    expect(markup).not.toContain('Hidden card goal should only appear after selecting the task')
    expect(markup).not.toContain('Hidden step one')
  })

  it('renders run and enable controls inside task list cards only', () => {
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

    const detailHead = markup.match(/<div class="scheduled-task-detail-head">[\s\S]*?<\/div>/)?.[0] ?? ''

    expect(markup).toContain('class="scheduled-task-card-actions"')
    expect(markup).toContain('data-task-action="run"')
    expect(markup).toContain('data-task-action="toggle"')
    expect(detailHead).not.toContain('data-task-action="run"')
    expect(detailHead).not.toContain('data-task-action="toggle"')
  })

  it('renders the selected task content as markdown', () => {
    const markup = renderToStaticMarkup(
      <ScheduledTaskDialog
        projectId="project-1"
        targetRepo={'E:\\OpenSource\\multi-ai-code'}
        sessionId="session-1"
        sessionRunning={true}
        initialTasks={[
          task({
            goal: '# Review plan\n\nCheck **risk** before release.\n\n- dependency updates'
          })
        ]}
        onClose={() => {}}
      />
    )

    expect(markup).toContain('scheduled-task-markdown')
    expect(markup).toContain('<h1>Review plan</h1>')
    expect(markup).toContain('<strong>risk</strong>')
    expect(markup).toContain('<li>dependency updates</li>')
  })
})
