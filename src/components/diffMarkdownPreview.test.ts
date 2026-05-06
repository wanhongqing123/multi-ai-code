import { describe, expect, it } from 'vitest'
import { buildMarkdownPreviewText, isMarkdownDiffPath } from './diffMarkdownPreview.js'

describe('isMarkdownDiffPath', () => {
  it('accepts .md and .markdown paths only', () => {
    expect(isMarkdownDiffPath('docs/guide.md')).toBe(true)
    expect(isMarkdownDiffPath('docs/guide.markdown')).toBe(true)
    expect(isMarkdownDiffPath('src/App.tsx')).toBe(false)
  })
})

describe('buildMarkdownPreviewText', () => {
  it('splits modified markdown diff lines into old and new text', () => {
    expect(
      buildMarkdownPreviewText([
        { kind: 'context', text: '# Title' },
        { kind: 'del', text: '- old item' },
        { kind: 'add', text: '- new item' },
        { kind: 'context', text: '' }
      ])
    ).toEqual({
      oldText: '# Title\n- old item\n',
      newText: '# Title\n- new item\n'
    })
  })

  it('handles added-only and removed-only files', () => {
    expect(
      buildMarkdownPreviewText([
        { kind: 'add', text: '# Added' },
        { kind: 'add', text: '' }
      ])
    ).toEqual({
      oldText: '',
      newText: '# Added\n'
    })

    expect(
      buildMarkdownPreviewText([
        { kind: 'del', text: '# Removed' },
        { kind: 'del', text: '' }
      ])
    ).toEqual({
      oldText: '# Removed\n',
      newText: ''
    })
  })
})
