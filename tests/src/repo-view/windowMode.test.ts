import { describe, expect, it } from 'vitest'
import { parseRendererWindowModeSearch } from '../../../src/repo-view/windowMode.js'

describe('parseRendererWindowModeSearch', () => {
  it('returns main mode for regular app window', () => {
    expect(parseRendererWindowModeSearch('')).toEqual({ kind: 'main' })
  })

  it('returns repo-view mode when projectId exists', () => {
    expect(parseRendererWindowModeSearch('?window=repo-view&projectId=p_1')).toEqual({
      kind: 'repo-view',
      projectId: 'p_1'
    })
  })

  // 内置截图功能已随「设置界面只保留 AICLI」一并删除，这两个窗口模式不再存在，
  // 老链接（?window=screenshot-*）必须退回主窗口而不是白屏。
  it('falls back to main for the removed screenshot window modes', () => {
    expect(parseRendererWindowModeSearch('?window=screenshot-overlay&token=abc123')).toEqual({
      kind: 'main'
    })
    expect(parseRendererWindowModeSearch('?window=screenshot-editor&token=xyz789')).toEqual({
      kind: 'main'
    })
    expect(parseRendererWindowModeSearch('?window=screenshot-overlay')).toEqual({ kind: 'main' })
    expect(parseRendererWindowModeSearch('?window=screenshot-editor')).toEqual({ kind: 'main' })
  })
})
