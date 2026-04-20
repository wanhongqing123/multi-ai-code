import { promises as fs } from 'fs'
import { basename, extname, isAbsolute, join } from 'path'

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

async function readDesignNames(targetRepo: string): Promise<string[]> {
  const dir = join(targetRepo, '.multi-ai-code', 'designs')
  try {
    const entries = await fs.readdir(dir)
    return entries
      .filter((f) => f.toLowerCase().endsWith('.md'))
      .map((f) => f.slice(0, -3))
  } catch {
    return []
  }
}

export async function listPlans(projectDir: string): Promise<PlanEntry[]> {
  const meta = await readMeta(projectDir)
  const targetRepo = meta.target_repo
  const planSources = meta.plan_sources ?? {}

  const externalEntries: PlanEntry[] = Object.entries(planSources).map(
    ([name, abs]) => ({ name, abs, source: 'external' as const })
  )
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
  const name = basename(externalAbsPath, '.md')
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
