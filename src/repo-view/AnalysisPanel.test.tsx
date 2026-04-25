import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import AnalysisPanel from './AnalysisPanel.js'

describe('AnalysisPanel', () => {
  it('shows the empty state when no file is selected', () => {
    const markup = renderToStaticMarkup(
      <AnalysisPanel
        filePath=""
        annotations={[]}
        activeAnnotationId={null}
        recentlyAddedAnnotationId={null}
        sessionRunning={true}
        sending={false}
        onSendToCli={vi.fn()}
        onEditAnnotation={vi.fn()}
        onRemoveAnnotation={vi.fn()}
        onClearAnnotations={vi.fn()}
      />
    )

    expect(markup).toContain('先从左侧选择一个文件')
  })

  it('marks active and recent annotations and shows tray copy', () => {
    const markup = renderToStaticMarkup(
      <AnalysisPanel
        filePath="src/repo-view/AnalysisPanel.tsx"
        annotations={[
          {
            id: 'ann_1',
            filePath: 'src/repo-view/AnalysisPanel.tsx',
            lineRange: '12-18',
            snippet: 'const value = true',
            comment: 'Check this branch.'
          }
        ]}
        activeAnnotationId="ann_1"
        recentlyAddedAnnotationId="ann_1"
        sessionRunning={true}
        sending={false}
        onSendToCli={vi.fn()}
        onEditAnnotation={vi.fn()}
        onRemoveAnnotation={vi.fn()}
        onClearAnnotations={vi.fn()}
      />
    )

    expect(markup).toContain('repo-analysis-item active recent')
    expect(markup).toContain('当前文件待发送批注')
    expect(markup).toContain('发送当前文件批注到 AI CLI')
  })

  it('shows the annotation tray hint when a file is selected but empty', () => {
    const markup = renderToStaticMarkup(
      <AnalysisPanel
        filePath="src/repo-view/AnalysisPanel.tsx"
        annotations={[]}
        activeAnnotationId={null}
        recentlyAddedAnnotationId={null}
        sessionRunning={true}
        sending={false}
        onSendToCli={vi.fn()}
        onEditAnnotation={vi.fn()}
        onRemoveAnnotation={vi.fn()}
        onClearAnnotations={vi.fn()}
      />
    )

    expect(markup).toContain('在代码区选中文本后点击')
    expect(markup).toContain('加入待发送批注托盘')
  })
})
