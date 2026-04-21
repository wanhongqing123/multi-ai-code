import { promises as fs } from 'fs'
import { basename, extname, isAbsolute, join } from 'path'

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

/** System/auto-generated files that live alongside design md's but are not
 *  user plans (e.g., Claude Code's auto-loaded CLAUDE.md, legacy Codex
 *  `.injections` file copies). Filename match is case-insensitive. */
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

  // Filter out dead external mappings — the user may have moved or deleted
  // the original .md file since registering it. Both:
  //   1. hide them from the plan list UI, AND
  //   2. prune them from project.json so they don't linger as ghosts.
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

/** Remove the given names from `project.json.plan_sources`. Best-effort —
 *  if the file is locked or unreadable we silently skip; the UI-layer
 *  filter in `listPlans` still hides the ghosts in this call's result. */
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
    /* tolerate failure — next listPlans call will try again */
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
  const existing = await listPlans(projectDir)
  if (existing.some((p) => p.name === name)) {
    return {
      ok: false,
      error: `已存在同名方案 "${name}"，请改源文件名后再导入`
    }
  }
  const metaPath = join(projectDir, 'project.json')
  let meta: Record<string, unknown> = {}
  try {
    meta = JSON.parse(await fs.readFile(metaPath, 'utf8'))
  } catch {
    /* empty */
  }
  const prev = (meta.plan_sources as Record<string, string> | undefined) ?? {}
  meta.plan_sources = { ...prev, [name]: externalAbsPath }
  meta.updated_at = new Date().toISOString()
  await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
  return { ok: true, name }
}
