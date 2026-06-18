import { createHash } from 'crypto'
import { homedir } from 'os'
import { basename, dirname, join, normalize, resolve } from 'path'
import { promises as fs } from 'fs'
import { rootDir } from '../store/paths.js'

export type LocalSkillHealth = 'ok' | 'missing-file' | 'invalid'
export type LocalSkillSourceKind = 'default' | 'custom'

export interface LocalSkillSource {
  id: string
  name: string
  path: string
  kind: LocalSkillSourceKind
  skillCount: number
  enabledCount: number
}

export interface LocalSkillPackage {
  id: string
  name: string
  description: string | null
  version: string | null
  dir: string
  skillFile: string
  sourceId: string
  sourceName: string
  sourcePath: string
  enabled: boolean
  health: LocalSkillHealth
  frontmatter: Record<string, string>
  markdown: string
  preview: string
  updatedAt: string | null
}

export interface LocalSkillSnapshot {
  sources: LocalSkillSource[]
  skills: LocalSkillPackage[]
  totals: {
    discovered: number
    enabled: number
    disabled: number
  }
  scannedAt: string
}

interface LocalSkillRegistryState {
  customRoots: string[]
  disabledSkillIds: string[]
  updatedAt: string
}

export interface LocalSkillRegistryOptions {
  defaultRoots?: string[]
  statePath?: string
  maxDepth?: number
}

const DEFAULT_STATE: LocalSkillRegistryState = {
  customRoots: [],
  disabledSkillIds: [],
  updatedAt: new Date(0).toISOString()
}

export function localSkillRegistryPath(): string {
  return join(rootDir(), 'skill-registry.json')
}

export function defaultLocalSkillRoots(home: string = homedir()): string[] {
  return [
    join(home, '.claude', 'plugins', 'cache'),
    join(home, '.codex', 'skills'),
    join(home, '.codex', 'superpowers', 'skills'),
    join(home, '.codex', 'plugins', 'cache'),
    join(home, '.agents', 'skills')
  ]
}

function stableId(prefix: string, value: string): string {
  return `${prefix}_${createHash('sha1').update(normalize(value).toLowerCase()).digest('hex').slice(0, 16)}`
}

function uniquePaths(paths: string[]): string[] {
  const seen = new Set<string>()
  const result: string[] = []
  for (const path of paths) {
    const resolved = resolve(path)
    const key = normalize(resolved).toLowerCase()
    if (seen.has(key)) continue
    seen.add(key)
    result.push(resolved)
  }
  return result
}

async function existsDir(path: string): Promise<boolean> {
  try {
    return (await fs.stat(path)).isDirectory()
  } catch {
    return false
  }
}

async function existsFile(path: string): Promise<boolean> {
  try {
    return (await fs.stat(path)).isFile()
  } catch {
    return false
  }
}

async function readState(statePath: string): Promise<LocalSkillRegistryState> {
  try {
    const raw = JSON.parse(await fs.readFile(statePath, 'utf8')) as Partial<LocalSkillRegistryState>
    return {
      customRoots: Array.isArray(raw.customRoots) ? raw.customRoots.filter((item) => typeof item === 'string') : [],
      disabledSkillIds: Array.isArray(raw.disabledSkillIds)
        ? raw.disabledSkillIds.filter((item) => typeof item === 'string')
        : [],
      updatedAt: typeof raw.updatedAt === 'string' ? raw.updatedAt : DEFAULT_STATE.updatedAt
    }
  } catch {
    return { ...DEFAULT_STATE }
  }
}

async function writeState(statePath: string, state: LocalSkillRegistryState): Promise<void> {
  await fs.mkdir(dirname(statePath), { recursive: true })
  await fs.writeFile(statePath, JSON.stringify(state, null, 2), 'utf8')
}

function splitFrontmatter(markdown: string): {
  frontmatter: Record<string, string>
  body: string
} {
  if (!markdown.startsWith('---')) return { frontmatter: {}, body: markdown.trim() }
  const end = markdown.indexOf('\n---', 3)
  if (end < 0) return { frontmatter: {}, body: markdown.trim() }
  const frontmatter: Record<string, string> = {}
  for (const line of markdown.slice(3, end).trim().split(/\r?\n/)) {
    const index = line.indexOf(':')
    if (index <= 0) continue
    const key = line.slice(0, index).trim()
    const value = line.slice(index + 1).trim().replace(/^['"]|['"]$/g, '')
    if (key) frontmatter[key] = value
  }
  return {
    frontmatter,
    body: markdown.slice(end + 4).trim()
  }
}

function sourceNameForRoot(root: string, kind: LocalSkillSourceKind): string {
  const normalized = normalize(root).toLowerCase()
  if (normalized.includes(`${normalize('.claude')}\\`) || normalized.includes('/.claude/')) {
    return 'Claude Skills'
  }
  if (normalized.includes(`${normalize('.codex')}\\`) || normalized.includes('/.codex/')) {
    return 'Codex Skills'
  }
  if (normalized.includes(`${normalize('.agents')}\\`) || normalized.includes('/.agents/')) {
    return 'Agents Skills'
  }
  return kind === 'custom' ? `自定义目录 · ${basename(root)}` : basename(root)
}

async function findSkillDirs(root: string, maxDepth: number): Promise<string[]> {
  if (!(await existsDir(root))) return []
  const found: string[] = []
  const queue: Array<{ dir: string; depth: number }> = [{ dir: root, depth: 0 }]
  const seen = new Set<string>()

  while (queue.length > 0) {
    const { dir, depth } = queue.shift()!
    const key = normalize(resolve(dir)).toLowerCase()
    if (seen.has(key)) continue
    seen.add(key)

    if (await existsFile(join(dir, 'SKILL.md'))) {
      found.push(dir)
      continue
    }
    if (depth >= maxDepth) continue

    let entries: Array<{ name: string; isDirectory: () => boolean }>
    try {
      entries = await fs.readdir(dir, { withFileTypes: true })
    } catch {
      continue
    }
    for (const entry of entries.sort((left, right) => left.name.localeCompare(right.name))) {
      if (!entry.isDirectory()) continue
      if (entry.name === 'node_modules' || entry.name === '.git') continue
      queue.push({ dir: join(dir, entry.name), depth: depth + 1 })
    }
  }

  return found
}

async function readSkillPackage(
  skillDir: string,
  source: Pick<LocalSkillSource, 'id' | 'name' | 'path'>,
  disabledIds: Set<string>
): Promise<LocalSkillPackage> {
  const skillFile = join(skillDir, 'SKILL.md')
  const id = stableId('skill', skillDir)
  try {
    const markdown = await fs.readFile(skillFile, 'utf8')
    const { frontmatter, body } = splitFrontmatter(markdown)
    const stat = await fs.stat(skillFile)
    const name = frontmatter.name || basename(skillDir)
    return {
      id,
      name,
      description: frontmatter.description || null,
      version: frontmatter.version || null,
      dir: skillDir,
      skillFile,
      sourceId: source.id,
      sourceName: source.name,
      sourcePath: source.path,
      enabled: !disabledIds.has(id),
      health: 'ok',
      frontmatter,
      markdown,
      preview: body.slice(0, 1400),
      updatedAt: stat.mtime.toISOString()
    }
  } catch {
    return {
      id,
      name: basename(skillDir),
      description: null,
      version: null,
      dir: skillDir,
      skillFile,
      sourceId: source.id,
      sourceName: source.name,
      sourcePath: source.path,
      enabled: !disabledIds.has(id),
      health: 'missing-file',
      frontmatter: {},
      markdown: '',
      preview: '',
      updatedAt: null
    }
  }
}

export async function scanLocalSkills(
  options: LocalSkillRegistryOptions = {}
): Promise<LocalSkillSnapshot> {
  const statePath = options.statePath ?? localSkillRegistryPath()
  const state = await readState(statePath)
  const disabledIds = new Set(state.disabledSkillIds)
  const defaultRoots = uniquePaths(options.defaultRoots ?? defaultLocalSkillRoots())
  const customRoots = uniquePaths(state.customRoots)
  const roots = [
    ...defaultRoots.map((path) => ({ path, kind: 'default' as const })),
    ...customRoots.map((path) => ({ path, kind: 'custom' as const }))
  ]

  const sources: LocalSkillSource[] = []
  const skills: LocalSkillPackage[] = []
  const seenSkillDirs = new Set<string>()

  for (const root of roots) {
    const source: LocalSkillSource = {
      id: stableId('source', `${root.kind}:${root.path}`),
      name: sourceNameForRoot(root.path, root.kind),
      path: root.path,
      kind: root.kind,
      skillCount: 0,
      enabledCount: 0
    }
    const dirs = await findSkillDirs(root.path, options.maxDepth ?? 8)
    for (const dir of dirs) {
      const key = normalize(resolve(dir)).toLowerCase()
      if (seenSkillDirs.has(key)) continue
      seenSkillDirs.add(key)
      const skill = await readSkillPackage(dir, source, disabledIds)
      skills.push(skill)
      source.skillCount += 1
      if (skill.enabled) source.enabledCount += 1
    }
    sources.push(source)
  }

  skills.sort((left, right) => left.name.localeCompare(right.name))
  const enabled = skills.filter((skill) => skill.enabled).length
  return {
    sources,
    skills,
    totals: {
      discovered: skills.length,
      enabled,
      disabled: skills.length - enabled
    },
    scannedAt: new Date().toISOString()
  }
}

export async function addLocalSkillSource(
  sourceDir: string,
  options: Pick<LocalSkillRegistryOptions, 'statePath'> = {}
): Promise<void> {
  const statePath = options.statePath ?? localSkillRegistryPath()
  const state = await readState(statePath)
  state.customRoots = uniquePaths([...state.customRoots, sourceDir])
  state.updatedAt = new Date().toISOString()
  await writeState(statePath, state)
}

export async function removeLocalSkillSource(
  sourceDir: string,
  options: Pick<LocalSkillRegistryOptions, 'statePath'> = {}
): Promise<void> {
  const statePath = options.statePath ?? localSkillRegistryPath()
  const removeKey = normalize(resolve(sourceDir)).toLowerCase()
  const state = await readState(statePath)
  state.customRoots = state.customRoots.filter(
    (root) => normalize(resolve(root)).toLowerCase() !== removeKey
  )
  state.updatedAt = new Date().toISOString()
  await writeState(statePath, state)
}

export async function setLocalSkillEnabled(
  skillId: string,
  enabled: boolean,
  options: Pick<LocalSkillRegistryOptions, 'statePath'> = {}
): Promise<void> {
  const statePath = options.statePath ?? localSkillRegistryPath()
  const state = await readState(statePath)
  const disabled = new Set(state.disabledSkillIds)
  if (enabled) {
    disabled.delete(skillId)
  } else {
    disabled.add(skillId)
  }
  state.disabledSkillIds = Array.from(disabled).sort()
  state.updatedAt = new Date().toISOString()
  await writeState(statePath, state)
}
