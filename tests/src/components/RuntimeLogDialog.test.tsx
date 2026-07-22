import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { RuntimeState } from '../../../electron/preload'
import RuntimeLogDialog from '../../../src/components/RuntimeLogDialog.js'

const runtimeState: RuntimeState = {
  status: 'running',
  projectId: 'project-1',
  projectName: 'Demo',
  targetRepo: 'E:/demo',
  cwd: 'E:/demo/build',
  command: 'apollodemo.exe',
  envType: 'visual-studio',
  visualStudioInstanceId: 'vs-2022',
  visualStudioDisplayName: 'Visual Studio 2022',
  outputEncoding: 'utf8',
  startedAt: '2026-06-16T08:00:00.000Z',
  finishedAt: null,
  exitCode: null,
  signal: null,
  log: '[apollo] video open failed'
}

describe('RuntimeLogDialog', () => {
  it('renders as a non-modal floating panel so the page behind remains interactive', () => {
    const markup = renderToStaticMarkup(
      <RuntimeLogDialog
        open={true}
        currentProjectName="Demo"
        currentProjectId="project-1"
        runtimeState={runtimeState}
        sessionId="session-1"
        sessionStatus="running"
        comment=""
        onCommentChange={vi.fn()}
        onClose={vi.fn()}
        onStopRuntime={vi.fn()}
        onSendRuntimeLog={vi.fn()}
      />
    )

    expect(markup).toContain('runtime-log-dialog-layer')
    expect(markup).not.toContain('runtime-log-dialog-overlay')
    expect(markup).toContain('aria-modal="false"')
  })

  it('renders live runtime logs and a comment box for analysis requests', () => {
    const markup = renderToStaticMarkup(
      <RuntimeLogDialog
        open={true}
        currentProjectName="Demo"
        currentProjectId="project-1"
        runtimeState={runtimeState}
        sessionId="session-1"
        sessionStatus="running"
        comment="帮我分析下这个日志，然后解释下为什么视频没有播放出来。"
        onCommentChange={vi.fn()}
        onClose={vi.fn()}
        onStopRuntime={vi.fn()}
        onSendRuntimeLog={vi.fn()}
      />
    )

    expect(markup).toContain('运行日志')
    expect(markup).toContain('[apollo] video open failed')
    expect(markup).toContain('textarea')
    expect(markup).toContain('为什么视频没有播放出来')
    expect(markup).toContain('发送分析')
    expect(markup).toContain('停止运行')
  })

  it('renders a runtime log filter control with a line summary', () => {
    const markup = renderToStaticMarkup(
      <RuntimeLogDialog
        open={true}
        currentProjectName="Demo"
        currentProjectId="project-1"
        runtimeState={{
          ...runtimeState,
          log: '[apollo] video open failed\n[DLManager] create task'
        }}
        sessionId="session-1"
        sessionStatus="running"
        comment=""
        onCommentChange={vi.fn()}
        onClose={vi.fn()}
        onStopRuntime={vi.fn()}
        onSendRuntimeLog={vi.fn()}
      />
    )

    expect(markup).toContain('runtime-log-filter')
    expect(markup).toContain('过滤日志')
    expect(markup).toContain('共 2 行')
  })

  it('renders move and edge resize handles for the floating window', () => {
    const markup = renderToStaticMarkup(
      <RuntimeLogDialog
        open={true}
        currentProjectName="Demo"
        currentProjectId="project-1"
        runtimeState={runtimeState}
        sessionId="session-1"
        sessionStatus="running"
        comment=""
        onCommentChange={vi.fn()}
        onClose={vi.fn()}
        onStopRuntime={vi.fn()}
        onSendRuntimeLog={vi.fn()}
      />
    )

    expect(markup).toContain('data-drag-handle="runtime-log-dialog"')
    for (const edge of ['n', 's', 'e', 'w', 'ne', 'nw', 'se', 'sw']) {
      expect(markup).toContain(`runtime-log-resize-handle-${edge}`)
    }
  })

  it('does not render when closed', () => {
    const markup = renderToStaticMarkup(
      <RuntimeLogDialog
        open={false}
        currentProjectName="Demo"
        currentProjectId="project-1"
        runtimeState={runtimeState}
        sessionId="session-1"
        sessionStatus="running"
        comment=""
        onCommentChange={vi.fn()}
        onClose={vi.fn()}
        onStopRuntime={vi.fn()}
        onSendRuntimeLog={vi.fn()}
      />
    )

    expect(markup).toBe('')
  })
})
