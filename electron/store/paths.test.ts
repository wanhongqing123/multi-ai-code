import { describe, it, expect, afterEach, beforeEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { designArchiveDir, createProjectLayout, ensureRootDir } from './paths.js'

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
})

describe('ensureRootDir', () => {
  it('removes existing workspaces/ dir in each project', async () => {
    const projRoot = await fs.mkdtemp(join(tmpdir(), 'mac-paths-ensure-'))
    process.env.MULTI_AI_ROOT = projRoot
    try {
      const pid = 'p_legacy'
      const pdir = join(projRoot, 'projects', pid, 'workspaces', 'stage1_design')
      await fs.mkdir(pdir, { recursive: true })
      await fs.writeFile(join(pdir, 'old.md'), 'legacy content')
      await ensureRootDir()
      const wsStat = await fs.stat(join(projRoot, 'projects', pid, 'workspaces')).catch(() => null)
      expect(wsStat).toBeNull()
    } finally {
      delete process.env.MULTI_AI_ROOT
      await fs.rm(projRoot, { recursive: true, force: true })
    }
  })

  it('is a no-op when project has no workspaces/ dir', async () => {
    const projRoot = await fs.mkdtemp(join(tmpdir(), 'mac-paths-noop-'))
    process.env.MULTI_AI_ROOT = projRoot
    try {
      await fs.mkdir(join(projRoot, 'projects', 'p_clean'), { recursive: true })
      await expect(ensureRootDir()).resolves.toBeUndefined()
    } finally {
      delete process.env.MULTI_AI_ROOT
      await fs.rm(projRoot, { recursive: true, force: true })
    }
  })
})
