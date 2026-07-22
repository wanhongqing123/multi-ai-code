import { describe, expect, it } from 'vitest'
import { buildDroppedFileInput } from '../../../src/components/terminalDragDrop.js'

describe('buildDroppedFileInput', () => {
  it('joins resolved file paths in drop order', () => {
    const files: Array<{ id: string; path?: string }> = [{ id: 'a' }, { id: 'b' }]
    const result = buildDroppedFileInput(
      files,
      (file) => `C:/repo/${file.id}.txt`
    )
    expect(result).toBe('C:/repo/a.txt C:/repo/b.txt')
  })

  it('falls back to file.path and skips empty items', () => {
    const files = [
      { path: 'C:/repo/a.txt' },
      {},
      { path: '' },
      { path: 'C:/repo/b.txt' }
    ]
    expect(buildDroppedFileInput(files)).toBe(
      'C:/repo/a.txt C:/repo/b.txt'
    )
  })

  it('returns an empty string when no usable path exists', () => {
    expect(buildDroppedFileInput([{}, { path: '' }])).toBe('')
  })
})
