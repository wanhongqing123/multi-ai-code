import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import {
  createInternalPlan,
  listPlans,
  registerExternalPlan,
  updatePlanMetadata,
  updatePlanDescription
} from '../../../electron/orchestrator/plans.js'

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
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: {
          alive,
          'ghost-plan': dead,
          'another-ghost': '/definitely/not/a/path/x.md'
        }
      })
    )

    const items = await listPlans(projectDir)

    expect(items.map((i) => i.name)).toEqual(['alive'])
    const meta = JSON.parse(await fs.readFile(join(projectDir, 'project.json'), 'utf8'))
    expect(meta.plan_sources).toEqual({ alive })
    await fs.rm(alive)
  })

  it('tolerates project.json write failure when pruning (still filters)', async () => {
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

  it('treats re-importing the same external entry as idempotent', async () => {
    const ext = join(tmpdir(), 'mac-reg-dup.md')
    await fs.writeFile(ext, '#')
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: { 'mac-reg-dup': ext }
      })
    )

    const r = await registerExternalPlan(projectDir, ext)

    expect(r).toEqual({ ok: true, name: 'mac-reg-dup' })
    const meta = JSON.parse(await fs.readFile(join(projectDir, 'project.json'), 'utf8'))
    expect(meta.plan_sources['mac-reg-dup']).toBe(ext)
    await fs.rm(ext)
  })

  it('rejects when a different external file has the same plan name', async () => {
    const existingExt = join(tmpdir(), 'mac-reg-name-conflict.md')
    const incomingDir = await fs.mkdtemp(join(tmpdir(), 'mac-reg-name-conflict-'))
    const incomingExt = join(incomingDir, 'mac-reg-name-conflict.md')
    await fs.writeFile(existingExt, '# existing')
    await fs.writeFile(incomingExt, '# incoming')
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: { 'mac-reg-name-conflict': existingExt }
      })
    )

    const r = await registerExternalPlan(projectDir, incomingExt)

    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/已存在同名方案/)
    await fs.rm(existingExt)
    await fs.rm(incomingExt)
    await fs.rm(incomingDir, { recursive: true, force: true })
  })
})

describe('createInternalPlan', () => {
  let projectDir: string
  let targetRepo: string

  beforeEach(async () => {
    projectDir = await fs.mkdtemp(join(tmpdir(), 'mac-create-plan-pdir-'))
    targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-create-plan-repo-'))
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({ id: 'p', name: 'p', target_repo: targetRepo })
    )
  })

  afterEach(async () => {
    await fs.rm(projectDir, { recursive: true, force: true })
    await fs.rm(targetRepo, { recursive: true, force: true })
  })

  it('creates an internal normal task markdown file and lists it', async () => {
    const result = await createInternalPlan(projectDir, 'Fix login flow')

    expect(result.ok).toBe(true)
    if (!result.ok) return
    expect(result.name).toBe('Fix_login_flow')
    expect(result.abs).toBe(join(targetRepo, '.multi-ai-code', 'designs', 'Fix_login_flow.md'))
    expect(await fs.readFile(result.abs, 'utf8')).toContain('# Fix login flow')

    const items = await listPlans(projectDir)
    expect(items).toEqual([
      {
        name: 'Fix_login_flow',
        abs: result.abs,
        source: 'internal'
      }
    ])
  })

  it('persists normal task descriptions in project metadata', async () => {
    const created = await createInternalPlan(projectDir, 'Fix login flow')
    expect(created.ok).toBe(true)

    const updated = await updatePlanDescription(
      projectDir,
      'Fix_login_flow',
      '## 登录修复\n\n- 补充重试策略'
    )

    expect(updated.ok).toBe(true)
    const items = await listPlans(projectDir)
    expect(items.find((item) => item.name === 'Fix_login_flow')?.description).toBe(
      '## 登录修复\n\n- 补充重试策略'
    )

    const meta = JSON.parse(await fs.readFile(join(projectDir, 'project.json'), 'utf8'))
    expect(meta.normal_task_descriptions.Fix_login_flow).toBe(
      '## 登录修复\n\n- 补充重试策略'
    )
  })

  it('persists normal task description and details independently', async () => {
    const created = await createInternalPlan(projectDir, 'Fix login flow')
    expect(created.ok).toBe(true)

    const updated = await updatePlanMetadata(projectDir, 'Fix_login_flow', {
      description: 'Login retry summary',
      details: '## Login retry details\n\n- Keep the session alive'
    })

    expect(updated.ok).toBe(true)
    const items = await listPlans(projectDir)
    expect(items.find((item) => item.name === 'Fix_login_flow')).toMatchObject({
      description: 'Login retry summary',
      details: '## Login retry details\n\n- Keep the session alive'
    })

    const meta = JSON.parse(await fs.readFile(join(projectDir, 'project.json'), 'utf8'))
    expect(meta.normal_task_descriptions.Fix_login_flow).toBe('Login retry summary')
    expect(meta.normal_task_details.Fix_login_flow).toBe(
      '## Login retry details\n\n- Keep the session alive'
    )
  })

  it('allows the legacy description updater to persist details for preload compatibility', async () => {
    const created = await createInternalPlan(projectDir, 'Fix login flow')
    expect(created.ok).toBe(true)

    const updated = await updatePlanDescription(
      projectDir,
      'Fix_login_flow',
      'Legacy summary',
      'Legacy detail body'
    )

    expect(updated.ok).toBe(true)
    const items = await listPlans(projectDir)
    expect(items.find((item) => item.name === 'Fix_login_flow')).toMatchObject({
      description: 'Legacy summary',
      details: 'Legacy detail body'
    })
  })

  it('rejects duplicate internal normal task names', async () => {
    const first = await createInternalPlan(projectDir, 'Fix login flow')
    expect(first.ok).toBe(true)

    const second = await createInternalPlan(projectDir, 'Fix login flow')

    expect(second.ok).toBe(false)
    if (!second.ok) expect(second.error).toContain('已存在')
  })

  it('rejects blank names', async () => {
    const result = await createInternalPlan(projectDir, '  ')

    expect(result.ok).toBe(false)
    if (!result.ok) expect(result.error).toContain('任务名')
  })
})
