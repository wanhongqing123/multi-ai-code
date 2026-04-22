import { describe, expect, it } from 'vitest'
import { shouldIgnoreRepoEntry, sortRepoEntries } from './filesystem.js'

describe('shouldIgnoreRepoEntry', () => {
  it('ignores heavy directories at any depth', () => {
    expect(shouldIgnoreRepoEntry('.git', true)).toBe(true)
    expect(shouldIgnoreRepoEntry('node_modules', true)).toBe(true)
    expect(shouldIgnoreRepoEntry('dist', true)).toBe(true)
    expect(shouldIgnoreRepoEntry('build', true)).toBe(true)
    expect(shouldIgnoreRepoEntry('out', true)).toBe(true)
  })

  it('keeps ordinary source directories and files', () => {
    expect(shouldIgnoreRepoEntry('src', true)).toBe(false)
    expect(shouldIgnoreRepoEntry('main.ts', false)).toBe(false)
  })
})

describe('sortRepoEntries', () => {
  it('sorts directories first and then by name', () => {
    expect(
      sortRepoEntries([
        { name: 'z.ts', isDirectory: false },
        { name: 'src', isDirectory: true },
        { name: 'a.ts', isDirectory: false },
        { name: 'assets', isDirectory: true }
      ]).map((x) => x.name)
    ).toEqual(['assets', 'src', 'a.ts', 'z.ts'])
  })
})
