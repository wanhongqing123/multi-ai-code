import { describe, expect, it } from 'vitest'
import { DIFF_MODE_TABS, diffModeLabel } from './diffViewerConfig.js'

describe('DIFF_MODE_TABS', () => {
  it('exposes the three active diff modes', () => {
    expect(DIFF_MODE_TABS).toEqual(['working', 'head1', 'commit'])
  })
})

describe('diffModeLabel', () => {
  it('returns stable labels for the visible diff modes', () => {
    expect(DIFF_MODE_TABS.map(diffModeLabel)).toEqual([
      '📝 当前改动',
      '⏱ 最近一次 commit',
      '🎯 指定 commit'
    ])
  })
})
