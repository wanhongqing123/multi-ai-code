import { describe, it, expect, afterEach, beforeEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import {
  createProjectLayout,
  ensureRootDir,
  setActiveAccount
} from '../../../electron/store/paths.js'

describe('createProjectLayout', () => {
  let root: string
  let targetRepo: string
  beforeEach(async () => {
    root = await fs.mkdtemp(join(tmpdir(), 'mac-paths-'))
    process.env.MULTI_AI_ROOT = root
    setActiveAccount('test-account')
    targetRepo = join(root, 'target')
    await fs.mkdir(targetRepo, { recursive: true })
  })
  afterEach(async () => {
    setActiveAccount(null)
    delete process.env.MULTI_AI_ROOT
    await fs.rm(root, { recursive: true, force: true })
  })

  // 普通任务删除后不该再往用户仓库里预建目录：没建过任务的仓库应当干干净净。
  it('does not create .multi-ai-code in the target repo', async () => {
    await createProjectLayout('p_test', targetRepo)
    await expect(fs.stat(join(targetRepo, '.multi-ai-code'))).rejects.toThrow()
  })
})

describe('ensureRootDir', () => {
  it('removes existing workspaces/ dir in each project', async () => {
    const projRoot = await fs.mkdtemp(join(tmpdir(), 'mac-paths-ensure-'))
    process.env.MULTI_AI_ROOT = projRoot
    setActiveAccount('test-account')
    try {
      const pid = 'p_legacy'
      const accountRoot = join(projRoot, 'accounts', 'test-account')
      const pdir = join(accountRoot, 'projects', pid, 'workspaces', 'stage1_design')
      await fs.mkdir(pdir, { recursive: true })
      await fs.writeFile(join(pdir, 'old.md'), 'legacy content')
      await ensureRootDir()
      const wsStat = await fs
        .stat(join(accountRoot, 'projects', pid, 'workspaces'))
        .catch(() => null)
      expect(wsStat).toBeNull()
    } finally {
      setActiveAccount(null)
      delete process.env.MULTI_AI_ROOT
      await fs.rm(projRoot, { recursive: true, force: true })
    }
  })

  it('is a no-op when project has no workspaces/ dir', async () => {
    const projRoot = await fs.mkdtemp(join(tmpdir(), 'mac-paths-noop-'))
    process.env.MULTI_AI_ROOT = projRoot
    setActiveAccount('test-account')
    try {
      await fs.mkdir(join(projRoot, 'accounts', 'test-account', 'projects', 'p_clean'), {
        recursive: true
      })
      await expect(ensureRootDir()).resolves.toBeUndefined()
    } finally {
      setActiveAccount(null)
      delete process.env.MULTI_AI_ROOT
      await fs.rm(projRoot, { recursive: true, force: true })
    }
  })
})
