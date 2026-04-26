import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import DiffViewerDialog from './DiffViewerDialog.js'

describe('DiffViewerDialog', () => {
  it('does not render the redundant close-without-send footer button', () => {
    const markup = renderToStaticMarkup(
      <DiffViewerDialog
        cwd="E:/OpenSource/multi-ai-code"
        onClose={vi.fn()}
        onSubmit={vi.fn()}
        sessionRunning={true}
        annotations={[]}
        onAnnotationsChange={vi.fn()}
        generalNote=""
        onGeneralNoteChange={vi.fn()}
        mode="working"
        onModeChange={vi.fn()}
        selectedCommit=""
        onSelectedCommitChange={vi.fn()}
        selectedFile=""
        onSelectedFileChange={vi.fn()}
      />
    )

    expect(markup).not.toContain('关闭（不发送）')
  })
})
