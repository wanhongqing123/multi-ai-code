import { describe, expect, it } from 'vitest'
import {
  filterRuntimeLogLines,
  formatRuntimeLogFilterSummary,
} from '../../../src/utils/runtimeLogFilter.js'

describe('runtime log filtering', () => {
  it('returns the original log when the filter is blank', () => {
    const log = '[apollo] start\n[DLManager] create task\n[video] render ok'

    expect(filterRuntimeLogLines(log, '   ')).toEqual({
      text: log,
      matchedLines: 3,
      totalLines: 3,
      active: false,
    })
  })

  it('keeps lines that match any whitespace-separated filter term case-insensitively', () => {
    const result = filterRuntimeLogLines(
      '[apollo] start\n[DLManager] create task\n[video] render ok\n[net] retry',
      'dlmanager VIDEO'
    )

    expect(result).toEqual({
      text: '[DLManager] create task\n[video] render ok',
      matchedLines: 2,
      totalLines: 4,
      active: true,
    })
  })

  it('formats a compact match summary for the toolbar', () => {
    expect(formatRuntimeLogFilterSummary({ active: false, matchedLines: 4, totalLines: 4 })).toBe(
      '共 4 行'
    )
    expect(formatRuntimeLogFilterSummary({ active: true, matchedLines: 2, totalLines: 4 })).toBe(
      '匹配 2 / 4 行'
    )
  })
})
