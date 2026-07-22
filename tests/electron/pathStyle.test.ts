import { describe, expect, it } from 'vitest'
import { joinWithRootStyle } from '../../electron/pathStyle.js'

describe('joinWithRootStyle', () => {
  it('preserves forward-slash roots', () => {
    expect(joinWithRootStyle('/tmp/repo', '.multi-ai-code', 'designs')).toBe(
      '/tmp/repo/.multi-ai-code/designs'
    )
    expect(joinWithRootStyle('/tmp/repo/', '.multi-ai-code', 'designs')).toBe(
      '/tmp/repo/.multi-ai-code/designs'
    )
  })

  it('preserves backslash roots', () => {
    expect(joinWithRootStyle('C:\\repo', '.multi-ai-code', 'designs')).toBe(
      'C:\\repo\\.multi-ai-code\\designs'
    )
    expect(joinWithRootStyle('C:\\repo\\', '.multi-ai-code', 'designs')).toBe(
      'C:\\repo\\.multi-ai-code\\designs'
    )
  })
})
