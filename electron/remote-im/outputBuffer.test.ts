import { describe, expect, it } from 'vitest'
import { createOutputChunks, stripTerminalControl } from './outputBuffer.js'

describe('remote IM output buffer', () => {
  it('strips ANSI and terminal control sequences', () => {
    expect(stripTerminalControl('\u001b[31mError\u001b[0m\r\n\u001b[?25lDone')).toBe(
      'Error\nDone'
    )
  })

  it('splits long clean output into max-sized chunks', () => {
    expect(createOutputChunks('abcdef', { maxChunkChars: 2 })).toEqual(['ab', 'cd', 'ef'])
  })

  it('drops empty chunks after cleanup', () => {
    expect(createOutputChunks('\u001b[0m\r\n', { maxChunkChars: 10 })).toEqual([])
  })
})
