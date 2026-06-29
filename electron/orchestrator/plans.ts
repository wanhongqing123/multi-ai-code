import { promises as fs } from 'fs'
import { basename, dirname, extname, isAbsolute, join } from 'path'
import { planArtifactPath } from './prompts.js'

function nameFromMdPath(absPath: string): string {
  return basename(absPath).replace(/\.md$/i, '')
}

export interface PlanEntry {
  name: string
  abs: string
  source: 'internal' | 'external'
}

interface ProjectMeta {
  target_repo?: string
  plan_sources?: Record<string, string>
}

async function readMeta(projectDir: string): Promise<ProjectMeta> {
  try {
    return JSON.parse(
      await fs.readFile(join(projectDir, 'project.json'), 'utf8')
    ) as ProjectMeta
  } catch {
    return {}
  }
}

// System/auto-generated files that live alongside design md files but are
// not user plans.
const RESERVED_DESIGN_NAMES = new Set<string>(['claude', 'codex', 'agents'])

async function readDesignNames(targetRepo: string): Promise<string[]> {
  const dir = join(targetRepo, '.multi-ai-code', 'designs')
  try {
    const entries = await fs.readdir(dir)
    return entries
      .filter((f) => f.toLowerCase().endsWith('.md'))
      .map((f) => f.slice(0, -3))
      .filter((name) => !RESERVED_DESIGN_NAMES.has(name.toLowerCase()))
  } catch {
    return []
  }
}

export async function listPlans(projectDir: string): Promise<PlanEntry[]> {
  const meta = await readMeta(projectDir)
  const targetRepo = meta.target_repo
  const planSources = meta.plan_sources ?? {}

  // Filter out dead external mappings. The user may have moved or deleted
  // the original file since registering it. Hide them in the UI and prune
  // them from project.json opportunistically.
  const externalEntries: PlanEntry[] = []
  const deadNames: string[] = []
  for (const [name, abs] of Object.entries(planSources)) {
    try {
      await fs.access(abs)
      externalEntries.push({ name, abs, source: 'external' })
    } catch {
      deadNames.push(name)
    }
  }
  if (deadNames.length > 0) {
    await pruneDeadPlanSources(projectDir, deadNames)
  }
  const externalNames = new Set(externalEntries.map((e) => e.name))

  const internalEntries: PlanEntry[] = []
  if (targetRepo) {
    const names = await readDesignNames(targetRepo)
    for (const name of names) {
      if (externalNames.has(name)) continue
      internalEntries.push({
        name,
        abs: join(targetRepo, '.multi-ai-code', 'designs', `${name}.md`),
        source: 'internal'
      })
    }
  }

  return [...internalEntries, ...externalEntries].sort((a, b) =>
    a.name.localeCompare(b.name)
  )
}

export async function createInternalPlan(
  projectDir: string,
  name: string
): Promise<{ ok: true; name: string; abs: string } | { ok: false; error: string }> {
  const displayName = name.trim()
  if (!displayName) {
    return { ok: false, error: '任务名不能为空' }
  }

  const meta = await readMeta(projectDir)
  if (!meta.target_repo) {
    return { ok: false, error: 'project.json missing target_repo' }
  }

  const abs = planArtifactPath(displayName, meta.target_repo)
  if (!abs) {
    return { ok: false, error: '无法创建普通任务文档路径' }
  }
  const safeName = nameFromMdPath(abs)

  try {
    await fs.access(abs)
    return { ok: false, error: `普通任务 "${safeName}" 已存在` }
  } catch {
    // Missing is the expected path for a new task.
  }

  await fs.mkdir(dirname(abs), { recursive: true })
  await fs.writeFile(abs, `# ${displayName}\n\n`, 'utf8')
  return { ok: true, name: safeName, abs }
}

// Remove the given names from project.json.plan_sources. Best effort only.
async function pruneDeadPlanSources(
  projectDir: string,
  names: string[]
): Promise<void> {
  const metaPath = join(projectDir, 'project.json')
  try {
    const raw = await fs.readFile(metaPath, 'utf8')
    const meta = JSON.parse(raw) as Record<string, unknown>
    const sources = meta.plan_sources as Record<string, string> | undefined
    if (!sources) return
    let changed = false
    for (const name of names) {
      if (name in sources) {
        delete sources[name]
        changed = true
      }
    }
    if (!changed) return
    meta.plan_sources = sources
    meta.updated_at = new Date().toISOString()
    await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
  } catch {
    // tolerate failure; next listPlans call will try again
  }
}

/**
 * Explicitly remove a single external plan mapping from project.json.
 * Returns ok even when the name is not present.
 */
export async function removeExternalPlan(
  projectDir: string,
  name: string
): Promise<{ ok: true } | { ok: false; error: string }> {
  const metaPath = join(projectDir, 'project.json')
  try {
    const raw = await fs.readFile(metaPath, 'utf8')
    const meta = JSON.parse(raw) as Record<string, unknown>
    const sources = meta.plan_sources as Record<string, string> | undefined
    if (sources && name in sources) {
      delete sources[name]
      meta.plan_sources = sources
      meta.updated_at = new Date().toISOString()
      await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
    }
    return { ok: true }
  } catch (err: unknown) {
    return {
      ok: false,
      error: err instanceof Error ? err.message : String(err)
    }
  }
}

export async function registerExternalPlan(
  projectDir: string,
  externalAbsPath: string
): Promise<{ ok: true; name: string } | { ok: false; error: string }> {
  if (!isAbsolute(externalAbsPath)) {
    return { ok: false, error: '必须是绝对路径 (absolute)' }
  }
  if (extname(externalAbsPath).toLowerCase() !== '.md') {
    return { ok: false, error: '仅支持 .md 文件' }
  }
  try {
    await fs.access(externalAbsPath)
  } catch {
    return { ok: false, error: `文件不存在 (does not exist): ${externalAbsPath}` }
  }

  const name = nameFromMdPath(externalAbsPath)
  const meta = await readMeta(projectDir)
  const existingSource = meta.plan_sources?.[name]

  // Re-importing the exact same external plan should be a no-op.
  if (existingSource === externalAbsPath) {
    return { ok: true, name }
  }

  const existing = await listPlans(projectDir)
  if (existing.some((p) => p.name === name)) {
    return {
      ok: false,
      error: `已存在同名方案 "${name}"，请改源文件名后再导入`
    }
  }

  const metaPath = join(projectDir, 'project.json')
  const nextMeta: Record<string, unknown> = { ...(meta as Record<string, unknown>) }
  const prev = (nextMeta.plan_sources as Record<string, string> | undefined) ?? {}
  nextMeta.plan_sources = { ...prev, [name]: externalAbsPath }
  nextMeta.updated_at = new Date().toISOString()
  await fs.writeFile(metaPath, JSON.stringify(nextMeta, null, 2))
  return { ok: true, name }
}
