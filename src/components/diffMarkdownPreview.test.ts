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
      newText: '# Title\n- new item\n',
      oldChangedLines: [2],
      newChangedLines: [2]
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
      newText: '# Added\n',
      oldChangedLines: [],
      newChangedLines: [1, 2]
    })

    expect(
      buildMarkdownPreviewText([
        { kind: 'del', text: '# Removed' },
        { kind: 'del', text: '' }
      ])
    ).toEqual({
      oldText: '# Removed\n',
      newText: '',
      oldChangedLines: [1, 2],
      newChangedLines: []
    })
  })

  it('tracks changed line numbers correctly when changes interleave with context', () => {
    const result = buildMarkdownPreviewText([
      { kind: 'context', text: '# Title' },     // old L1, new L1
      { kind: 'context', text: '' },            // old L2, new L2
      { kind: 'del', text: '- one' },           // old L3
      { kind: 'del', text: '- two' },           // old L4
      { kind: 'add', text: '- ONE' },           // new L3
      { kind: 'add', text: '- TWO' },           // new L4
      { kind: 'add', text: '- THREE' },         // new L5
      { kind: 'context', text: 'after' }        // old L5, new L6
    ])
    expect(result.oldChangedLines).toEqual([3, 4])
    expect(result.newChangedLines).toEqual([3, 4, 5])
  })
})
