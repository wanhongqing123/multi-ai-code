import { afterEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  ensureRepoMemoryExcluded,
  repoMemoryDir,
  repoMemoryFileNotePath,
  repoMemoryProjectSummaryPath
} from './memory.js'

describe('repo-memory paths', () => {
  it('maps repo root to the expected private memory paths', () => {
    const root = '/tmp/demo-repo'
    expect(repoMemoryDir(root)).toBe('/tmp/demo-repo/.multi-ai-code/repo-memory')
    expect(repoMemoryProjectSummaryPath(root)).toBe(
      '/tmp/demo-repo/.multi-ai-code/repo-memory/project-summary.md'
    )
    expect(repoMemoryFileNotePath(root, 'src/app.ts')).toContain(
      '/tmp/demo-repo/.multi-ai-code/repo-memory/file-notes/src/app.ts.md'
    )
  })
})

describe('ensureRepoMemoryExcluded', () => {
  const roots: string[] = []

  afterEach(async () => {
    await Promise.all(roots.map((x) => fs.rm(x, { recursive: true, force: true })))
  })

  it('appends repo-memory ignore only once', async () => {
    const root = await fs.mkdtemp(join(tmpdir(), 'repo-memory-'))
    roots.push(root)
    await fs.mkdir(join(root, '.git', 'info'), { recursive: true })
    await ensureRepoMemoryExcluded(root)
    await ensureRepoMemoryExcluded(root)
    const text = await fs.readFile(join(root, '.git', 'info', 'exclude'), 'utf8')
    expect(text.match(/\.multi-ai-code\/repo-memory\//g)?.length).toBe(1)
  })
})
