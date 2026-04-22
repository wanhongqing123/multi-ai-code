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
})
