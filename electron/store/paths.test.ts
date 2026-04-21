import { describe, it, expect, afterEach, beforeEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { designArchiveDir, createProjectLayout, ensureRootDir, migrateLegacyStage1Artifacts } from './paths.js'

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

describe('migrateLegacyStage1Artifacts', () => {
  it('migrateLegacyStage1Artifacts copies stage1 md into .multi-ai-code/designs/', async () => {
    const root = await fs.mkdtemp(join(tmpdir(), 'mac-migrate-'))
    process.env.MULTI_AI_ROOT = root
    try {
      const pid = 'p_legacy'
      const pdir = join(root, 'projects', pid)
      const legacyDir = join(pdir, 'workspaces', 'stage1_design')
      await fs.mkdir(legacyDir, { recursive: true })
      await fs.writeFile(join(legacyDir, 'my-plan.md'), '# plan body')
      const targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-migrate-repo-'))
      await fs.writeFile(
        join(pdir, 'project.json'),
        JSON.stringify({ id: pid, name: pid, target_repo: targetRepo })
      )
      await migrateLegacyStage1Artifacts()
      const migrated = await fs.readFile(
        join(targetRepo, '.multi-ai-code', 'designs', 'my-plan.md'),
        'utf8'
      )
      expect(migrated).toBe('# plan body')
      await fs.rm(targetRepo, { recursive: true, force: true })
    } finally {
      delete process.env.MULTI_AI_ROOT
      await fs.rm(root, { recursive: true, force: true })
    }
  })

  it('migrateLegacyStage1Artifacts skips when target already exists (no clobber)', async () => {
    const root = await fs.mkdtemp(join(tmpdir(), 'mac-migrate-skip-'))
    process.env.MULTI_AI_ROOT = root
    try {
      const pid = 'p_legacy2'
      const pdir = join(root, 'projects', pid)
      const legacyDir = join(pdir, 'workspaces', 'stage1_design')
      await fs.mkdir(legacyDir, { recursive: true })
      await fs.writeFile(join(legacyDir, 'my-plan.md'), '# OLD legacy content')
      const targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-migrate-skip-repo-'))
      const destDir = join(targetRepo, '.multi-ai-code', 'designs')
      await fs.mkdir(destDir, { recursive: true })
      await fs.writeFile(join(destDir, 'my-plan.md'), '# NEW current content')
      await fs.writeFile(
        join(pdir, 'project.json'),
        JSON.stringify({ id: pid, name: pid, target_repo: targetRepo })
      )
      await migrateLegacyStage1Artifacts()
      const content = await fs.readFile(join(destDir, 'my-plan.md'), 'utf8')
      expect(content).toBe('# NEW current content')
      await fs.rm(targetRepo, { recursive: true, force: true })
    } finally {
      delete process.env.MULTI_AI_ROOT
      await fs.rm(root, { recursive: true, force: true })
    }
  })
})
