import { describe, expect, it } from 'vitest'
import { parseRendererWindowModeSearch } from './windowMode.js'

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

  it('returns screenshot-overlay mode when token exists', () => {
    expect(
      parseRendererWindowModeSearch('?window=screenshot-overlay&token=abc123')
    ).toEqual({
      kind: 'screenshot-overlay',
      sessionToken: 'abc123'
    })
  })

  it('returns screenshot-editor mode when token exists', () => {
    expect(
      parseRendererWindowModeSearch('?window=screenshot-editor&token=xyz789')
    ).toEqual({
      kind: 'screenshot-editor',
      sessionToken: 'xyz789'
    })
  })

  it('falls back to main when screenshot window has no token', () => {
    expect(parseRendererWindowModeSearch('?window=screenshot-overlay')).toEqual({
      kind: 'main'
    })
    expect(parseRendererWindowModeSearch('?window=screenshot-editor')).toEqual({
      kind: 'main'
    })
  })
})
