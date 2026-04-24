import { describe, expect, it, beforeEach } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { mkdtemp } from 'fs/promises'
import { ensureAnalysisCacheDir } from './analysisCache'

async function makeRepo(): Promise<string> {
  return await mkdtemp(join(tmpdir(), 'multi-ai-code-test-'))
}

describe('ensureAnalysisCacheDir', () => {
  let repo: string

  beforeEach(async () => {
    repo = await makeRepo()
  })

  it('creates the analyses directory', async () => {
    await ensureAnalysisCacheDir(repo)
    const stat = await fs.stat(join(repo, '.multi-ai-code/repo-view/analyses'))
    expect(stat.isDirectory()).toBe(true)
  })

  it('creates a .gitignore with the cache rule when missing', async () => {
    await ensureAnalysisCacheDir(repo)
    const gi = await fs.readFile(join(repo, '.multi-ai-code/.gitignore'), 'utf8')
    expect(gi).toContain('repo-view/analyses/')
  })

  it('appends the rule when .gitignore exists without it', async () => {
    await fs.mkdir(join(repo, '.multi-ai-code'), { recursive: true })
    await fs.writeFile(join(repo, '.multi-ai-code/.gitignore'), 'foo\n', 'utf8')
    await ensureAnalysisCacheDir(repo)
    const gi = await fs.readFile(join(repo, '.multi-ai-code/.gitignore'), 'utf8')
    expect(gi).toContain('foo')
    expect(gi).toContain('repo-view/analyses/')
  })

  it('does not duplicate the rule when already present', async () => {
    await fs.mkdir(join(repo, '.multi-ai-code'), { recursive: true })
    await fs.writeFile(
      join(repo, '.multi-ai-code/.gitignore'),
      'foo\nrepo-view/analyses/\n',
      'utf8'
    )
    await ensureAnalysisCacheDir(repo)
    const gi = await fs.readFile(join(repo, '.multi-ai-code/.gitignore'), 'utf8')
    const occurrences = gi.split('repo-view/analyses/').length - 1
    expect(occurrences).toBe(1)
  })

  it('is idempotent', async () => {
    await ensureAnalysisCacheDir(repo)
    await ensureAnalysisCacheDir(repo)
    const gi = await fs.readFile(join(repo, '.multi-ai-code/.gitignore'), 'utf8')
    const occurrences = gi.split('repo-view/analyses/').length - 1
    expect(occurrences).toBe(1)
  })
})
