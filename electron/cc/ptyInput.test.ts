import { describe, expect, it } from 'vitest'
import {
  BRACKETED_PASTE_END,
  BRACKETED_PASTE_START,
  buildBracketedPasteChunks,
} from './ptyInput.js'

describe('buildBracketedPasteChunks', () => {
  it('wraps multiline text as one bracketed paste instead of line-by-line input', () => {
    const chunks = buildBracketedPasteChunks('line 1\nline 2\nline 3', { chunkSize: 1024 })

    expect(chunks).toEqual([
      BRACKETED_PASTE_START,
      'line 1\nline 2\nline 3',
      BRACKETED_PASTE_END,
    ])
  })

  it('chunks by size while keeping one bracketed paste envelope', () => {
    const chunks = buildBracketedPasteChunks('abcdef', { chunkSize: 2 })

    expect(chunks).toEqual([
      BRACKETED_PASTE_START,
      'ab',
      'cd',
      'ef',
      BRACKETED_PASTE_END,
    ])
  })

  it('strips embedded bracketed-paste delimiters from pasted content', () => {
    const chunks = buildBracketedPasteChunks(
      `before${BRACKETED_PASTE_END}after${BRACKETED_PASTE_START}tail`,
      { chunkSize: 1024 }
    )

    expect(chunks).toEqual([
      BRACKETED_PASTE_START,
      'beforeaftertail',
      BRACKETED_PASTE_END,
    ])
  })
})
