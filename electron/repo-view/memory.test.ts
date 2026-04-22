import { afterEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  ensureRepoMemoryExcluded,
  repoMemoryDir,
  repoMemoryFileNotePath,
  repoMemoryProjectSummaryPath,
  repoMemoryConversationHistoryPath,
  readRepoConversationHistory,
  writeRepoConversationHistory
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
    expect(repoMemoryConversationHistoryPath(root)).toBe(
      '/tmp/demo-repo/.multi-ai-code/repo-memory/repo-view-history.json'
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

  it('round-trips persisted repo-view conversation history', async () => {
    const root = await fs.mkdtemp(join(tmpdir(), 'repo-memory-'))
    roots.push(root)
    await fs.mkdir(join(root, '.git', 'info'), { recursive: true })

    await writeRepoConversationHistory(root, [
      { id: 'u1', role: 'user', text: '问题 1' },
      { id: 'a1', role: 'assistant', text: '回答 1' }
    ])

    const messages = await readRepoConversationHistory(root)
    expect(messages).toEqual([
      { id: 'u1', role: 'user', text: '问题 1' },
      { id: 'a1', role: 'assistant', text: '回答 1' }
    ])
  })
})
