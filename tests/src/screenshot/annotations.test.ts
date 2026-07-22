import { describe, expect, it } from 'vitest'
import { appendAnnotation, undoLast, type Annotation } from '../../../src/screenshot/annotations.js'

describe('annotation history helpers', () => {
  it('appendAnnotation returns a new array (does not mutate)', () => {
    const a: Annotation[] = []
    const next = appendAnnotation(a, {
      kind: 'rect',
      color: '#ef4444',
      x: 0,
      y: 0,
      w: 10,
      h: 10
    })
    expect(next).toHaveLength(1)
    expect(a).toHaveLength(0)
  })

  it('undoLast pops one entry', () => {
    const a: Annotation[] = [
      { kind: 'rect', color: '#ef4444', x: 0, y: 0, w: 10, h: 10 },
      { kind: 'text', color: '#111827', x: 5, y: 5, fontSize: 18, text: 'hi' }
    ]
    expect(undoLast(a)).toHaveLength(1)
  })

  it('undoLast on empty returns the same reference', () => {
    const a: Annotation[] = []
    expect(undoLast(a)).toBe(a)
  })
})
