import { describe, it, expect, afterEach, beforeEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { designArchiveDir, createProjectLayout, workspaceDir } from './paths.js'

describe('designArchiveDir', () => {
  it('returns <target_repo>/.multi-ai-code/designs', () => {
    expect(designArchiveDir('/tmp/my-repo')).toBe('/tmp/my-repo/.multi-ai-code/designs')
  })

  it('handles trailing slash on target_repo', () => {
    expect(designArchiveDir('/tmp/my-repo/')).toBe('/tmp/my-repo/.multi-ai-code/designs')
  })
})

describe('createProjectLayout', () => {
  let root: string
  let targetRepo: string
  beforeEach(async () => {
    root = await fs.mkdtemp(join(tmpdir(), 'mac-paths-'))
    process.env.MULTI_AI_ROOT = root
    targetRepo = join(root, 'target')
    await fs.mkdir(targetRepo, { recursive: true })
  })
  afterEach(async () => {
    delete process.env.MULTI_AI_ROOT
    await fs.rm(root, { recursive: true, force: true })
  })

  it('creates .multi-ai-code/designs under target_repo', async () => {
    await createProjectLayout('p_test', targetRepo)
    const stat = await fs.stat(designArchiveDir(targetRepo))
    expect(stat.isDirectory()).toBe(true)
  })

  it('still creates the isolated stage1_design workspace (used as cwd)', async () => {
    await createProjectLayout('p_test', targetRepo)
    const stat = await fs.stat(workspaceDir('p_test', 1))
    expect(stat.isDirectory()).toBe(true)
  })
})
