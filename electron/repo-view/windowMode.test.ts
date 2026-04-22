import { describe, expect, it } from 'vitest'
import { buildRepoViewSearch, parseWindowModeSearch } from './windowMode.js'

describe('parseWindowModeSearch', () => {
  it('defaults to main mode when no params are present', () => {
    expect(parseWindowModeSearch('')).toEqual({ kind: 'main' })
  })

  it('parses repo-view mode with projectId', () => {
    expect(parseWindowModeSearch('?window=repo-view&projectId=p_123')).toEqual({
      kind: 'repo-view',
      projectId: 'p_123'
    })
  })

  it('falls back to main mode when repo-view has no projectId', () => {
    expect(parseWindowModeSearch('?window=repo-view')).toEqual({ kind: 'main' })
  })
})

describe('buildRepoViewSearch', () => {
  it('creates a stable search string for repo-view windows', () => {
    expect(buildRepoViewSearch('p_abc')).toBe('?window=repo-view&projectId=p_abc')
  })
})
