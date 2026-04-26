import type { ComponentProps } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import AnalysisPanel, { type RepoCodeAnnotation } from './AnalysisPanel.js'

function renderPanel(
  overrides: Partial<ComponentProps<typeof AnalysisPanel>> = {}
): string {
  return renderToStaticMarkup(
    <AnalysisPanel
      filePath=""
      annotations={[]}
      sessionRunning={true}
      sending={false}
      onSendToCli={vi.fn().mockResolvedValue(true)}
      onEditAnnotation={vi.fn()}
      onRemoveAnnotation={vi.fn()}
      onClearAnnotations={vi.fn()}
      {...overrides}
    />
  )
}

describe('AnalysisPanel', () => {
  it('shows the empty state when no file is selected and there are no annotations', () => {
    const markup = renderPanel()

    expect(markup).toContain('先从左侧选择一个文件。')
  })

  it('renders annotations from multiple files with the short send label', () => {
    const annotations: RepoCodeAnnotation[] = [
      {
        id: 'ann_1',
        filePath: 'src/repo-view/AnalysisPanel.tsx',
        lineRange: '12-18',
        snippet: 'const value = true',
        comment: 'Check this branch.'
      },
      {
        id: 'ann_2',
        filePath: 'src/repo-view/RepoViewerWindow.tsx',
        lineRange: '302-336',
        snippet: 'const targetAnns = annotations',
        comment: 'Confirm all annotations are sent.'
      }
    ]

    const markup = renderPanel({
      filePath: 'src/repo-view/AnalysisPanel.tsx',
      annotations
    })

    expect(markup).toContain('repo-analysis-item')
    expect(markup).toContain('待发送标注（2）')
    expect(markup).toContain('发送')
    expect(markup).toContain('src/repo-view/AnalysisPanel.tsx')
    expect(markup).toContain('src/repo-view/RepoViewerWindow.tsx')
  })

  it('marks active and recent annotations and keeps cross-file entries visible', () => {
    const annotations: RepoCodeAnnotation[] = [
      {
        id: 'ann_1',
        filePath: 'src/repo-view/AnalysisPanel.tsx',
        lineRange: '12-18',
        snippet: 'const value = true',
        comment: 'Check this branch.'
      }
    ]

    const markup = renderPanel({
      filePath: '',
      annotations,
      activeAnnotationId: 'ann_1',
      recentlyAddedAnnotationId: 'ann_1'
    })

    expect(markup).toContain('repo-analysis-item active recent')
    expect(markup).toContain('待发送标注（1）')
    expect(markup).not.toContain('先从左侧选择一个文件。')
  })

  it('shows the annotation tray hint when a file is selected but empty', () => {
    const markup = renderPanel({ filePath: 'src/repo-view/AnalysisPanel.tsx' })

    expect(markup).toContain('在代码区选中文本后点击“标注”，即可加入待发送标注列表。')
  })
})
