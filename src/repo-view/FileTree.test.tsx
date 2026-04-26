import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import FileTree from './FileTree.js'

describe('FileTree', () => {
  it('renders file search input in the tree header', () => {
    const markup = renderToStaticMarkup(
      <FileTree
        repoRoot="E:/repo"
        selectedFile=""
        onSelectFile={vi.fn()}
      />
    )

    expect(markup).toContain('class="repo-tree-search-input"')
    expect(markup).toContain('placeholder="搜索文件"')
  })
})
