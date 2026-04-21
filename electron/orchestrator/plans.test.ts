import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { listPlans, registerExternalPlan } from './plans.js'

describe('listPlans', () => {
  let projectDir: string
  let targetRepo: string
  beforeEach(async () => {
    projectDir = await fs.mkdtemp(join(tmpdir(), 'mac-plans-pdir-'))
    targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-plans-repo-'))
    await fs.mkdir(join(targetRepo, '.multi-ai-code', 'designs'), { recursive: true })
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({ id: 'p', name: 'p', target_repo: targetRepo })
    )
  })
  afterEach(async () => {
    await fs.rm(projectDir, { recursive: true, force: true })
    await fs.rm(targetRepo, { recursive: true, force: true })
  })

  it('lists internal-only plans sorted alphabetically', async () => {
    await fs.writeFile(join(targetRepo, '.multi-ai-code', 'designs', 'beta.md'), '#')
    await fs.writeFile(join(targetRepo, '.multi-ai-code', 'designs', 'alpha.md'), '#')
    const items = await listPlans(projectDir)
    expect(items.map((i) => i.name)).toEqual(['alpha', 'beta'])
    expect(items.every((i) => i.source === 'internal')).toBe(true)
  })

  it('lists external-only plans from plan_sources', async () => {
    const ext1 = join(tmpdir(), 'mac-ext-foo.md')
    const ext2 = join(tmpdir(), 'mac-ext-bar.md')
    await fs.writeFile(ext1, '# foo')
    await fs.writeFile(ext2, '# bar')
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: { foo: ext1, bar: ext2 }
      })
    )
    const items = await listPlans(projectDir)
    expect(items.map((i) => i.name)).toEqual(['bar', 'foo'])
    expect(items.every((i) => i.source === 'external')).toBe(true)
    expect(items.find((i) => i.name === 'foo')?.abs).toBe(ext1)
    await fs.rm(ext1)
    await fs.rm(ext2)
  })

  it('external entry wins on name conflict', async () => {
    await fs.writeFile(join(targetRepo, '.multi-ai-code', 'designs', 'foo.md'), '# internal')
    const ext = join(tmpdir(), 'mac-ext-foo-conflict.md')
    await fs.writeFile(ext, '# external')
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: { foo: ext, bar: ext }
      })
    )
    const items = await listPlans(projectDir)
    expect(items).toEqual([
      { name: 'bar', abs: ext, source: 'external' },
      { name: 'foo', abs: ext, source: 'external' }
    ])
    await fs.rm(ext)
  })

  it('returns empty when designs dir missing AND no plan_sources', async () => {
    await fs.rm(join(targetRepo, '.multi-ai-code'), { recursive: true })
    const items = await listPlans(projectDir)
    expect(items).toEqual([])
  })

  it('filters dead external plans AND prunes them from project.json', async () => {
    const alive = join(tmpdir(), 'mac-listplans-alive.md')
    const dead = join(tmpdir(), 'mac-listplans-dead-never-existed.md')
    await fs.writeFile(alive, '# alive')
    // intentionally DO NOT create `dead` — it's a stale entry
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: {
          alive: alive,
          'ghost-plan': dead,
          'another-ghost': '/definitely/not/a/path/x.md'
        }
      })
    )
    const items = await listPlans(projectDir)
    expect(items.map((i) => i.name)).toEqual(['alive'])
    // project.json should have been rewritten to drop the two dead names.
    const meta = JSON.parse(
      await fs.readFile(join(projectDir, 'project.json'), 'utf8')
    )
    expect(meta.plan_sources).toEqual({ alive })
    await fs.rm(alive)
  })

  it('tolerates project.json write failure when pruning (still filters)', async () => {
    // Make project.json read-only so the prune write silently fails.
    const deadExt = '/definitely/no/such/path/ghost.md'
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: { ghost: deadExt }
      })
    )
    await fs.chmod(join(projectDir, 'project.json'), 0o444)
    try {
      const items = await listPlans(projectDir)
      expect(items).toEqual([])
    } finally {
      await fs.chmod(join(projectDir, 'project.json'), 0o644)
    }
  })

  it('returns empty when project.json missing', async () => {
    await fs.rm(join(projectDir, 'project.json'))
    const items = await listPlans(projectDir)
    expect(items).toEqual([])
  })
})

describe('registerExternalPlan', () => {
  let projectDir: string
  let targetRepo: string
  beforeEach(async () => {
    projectDir = await fs.mkdtemp(join(tmpdir(), 'mac-reg-pdir-'))
    targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-reg-repo-'))
    await fs.mkdir(join(targetRepo, '.multi-ai-code', 'designs'), { recursive: true })
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({ id: 'p', name: 'p', target_repo: targetRepo })
    )
  })
  afterEach(async () => {
    await fs.rm(projectDir, { recursive: true, force: true })
    await fs.rm(targetRepo, { recursive: true, force: true })
  })

  it('registers a new external plan and returns its name', async () => {
    const ext = join(tmpdir(), 'mac-reg-new.md')
    await fs.writeFile(ext, '# x')
    const r = await registerExternalPlan(projectDir, ext)
    expect(r).toEqual({ ok: true, name: 'mac-reg-new' })
    const meta = JSON.parse(await fs.readFile(join(projectDir, 'project.json'), 'utf8'))
    expect(meta.plan_sources['mac-reg-new']).toBe(ext)
    await fs.rm(ext)
  })

  it('rejects non-.md files', async () => {
    const ext = join(tmpdir(), 'mac-reg-not-md.txt')
    await fs.writeFile(ext, 'x')
    const r = await registerExternalPlan(projectDir, ext)
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/\.md/)
    await fs.rm(ext)
  })

  it('rejects nonexistent files', async () => {
    const r = await registerExternalPlan(projectDir, '/no/such/file.md')
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/not.*exist|找不到/i)
  })

  it('rejects relative paths', async () => {
    const r = await registerExternalPlan(projectDir, 'relative.md')
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/绝对|absolute/i)
  })

  it('rejects when name conflicts with an internal plan', async () => {
    await fs.writeFile(join(targetRepo, '.multi-ai-code', 'designs', 'dup.md'), '#')
    const ext = join(tmpdir(), 'dup.md')
    await fs.writeFile(ext, '#')
    const r = await registerExternalPlan(projectDir, ext)
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/已存在同名方案/)
    await fs.rm(ext)
  })

  it('strips the .MD extension case-insensitively', async () => {
    const ext = join(tmpdir(), 'mac-reg-uppercase.MD')
    await fs.writeFile(ext, '#')
    const r = await registerExternalPlan(projectDir, ext)
    expect(r).toEqual({ ok: true, name: 'mac-reg-uppercase' })
    await fs.rm(ext)
  })

  it('rejects when name conflicts with an existing external entry', async () => {
    const ext = join(tmpdir(), 'mac-reg-dup.md')
    await fs.writeFile(ext, '#')
    const meta = {
      id: 'p',
      name: 'p',
      target_repo: targetRepo,
      plan_sources: { 'mac-reg-dup': ext }
    }
    await fs.writeFile(join(projectDir, 'project.json'), JSON.stringify(meta))
    const r = await registerExternalPlan(projectDir, ext)
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/已存在同名方案/)
    await fs.rm(ext)
  })
})
