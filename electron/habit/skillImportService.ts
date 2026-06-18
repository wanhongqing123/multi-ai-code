import { promises as fs } from 'fs'
import { basename, join } from 'path'
import { createSkill, type CreateSkillInput } from './skills.js'

export interface SkillMarkdownFile {
  name: string
  dir: string
  skillFile: string
}

export interface ImportSkillsResult {
  ok: boolean
  imported: number
  skillIds: number[]
  error?: string
}

function slugify(value: string): string {
  return value
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_-]+/g, '-')
    .replace(/^-+|-+$/g, '')
}

function splitFrontmatter(markdown: string): {
  frontmatter: Record<string, string>
  body: string
} {
  if (!markdown.startsWith('---')) {
    return { frontmatter: {}, body: markdown.trim() }
  }
  const end = markdown.indexOf('\n---', 3)
  if (end < 0) return { frontmatter: {}, body: markdown.trim() }
  const rawFrontmatter = markdown.slice(3, end).trim()
  const body = markdown.slice(end + 4).trim()
  const frontmatter: Record<string, string> = {}
  for (const line of rawFrontmatter.split(/\r?\n/)) {
    const match = /^([A-Za-z0-9_-]+)\s*:\s*(.*)$/.exec(line)
    if (!match) continue
    frontmatter[match[1]] = match[2].replace(/^['"]|['"]$/g, '').trim()
  }
  return { frontmatter, body }
}

export function parseSkillMarkdown(markdown: string, fallbackName: string): CreateSkillInput {
  const { frontmatter, body } = splitFrontmatter(markdown)
  const name = frontmatter.name || fallbackName
  const trigger = slugify(fallbackName || name)
  return {
    name,
    description: frontmatter.description || null,
    trigger: trigger || null,
    source: 'imported',
    steps: [
      {
        type: 'prompt',
        text: body || markdown.trim()
      },
      { type: 'wait-response' }
    ]
  }
}

async function isFile(path: string): Promise<boolean> {
  try {
    return (await fs.stat(path)).isFile()
  } catch {
    return false
  }
}

export async function discoverSkillMarkdownFiles(root: string): Promise<SkillMarkdownFile[]> {
  const directSkillFile = join(root, 'SKILL.md')
  if (await isFile(directSkillFile)) {
    return [{ name: basename(root), dir: root, skillFile: directSkillFile }]
  }

  let entries: string[]
  try {
    entries = await fs.readdir(root)
  } catch {
    return []
  }

  const found: SkillMarkdownFile[] = []
  for (const entry of entries.sort((left, right) => left.localeCompare(right))) {
    const dir = join(root, entry)
    const skillFile = join(dir, 'SKILL.md')
    if (await isFile(skillFile)) {
      found.push({ name: entry, dir, skillFile })
    }
  }
  return found
}

export async function importSkillsFromDirectory(root: string): Promise<ImportSkillsResult> {
  try {
    const files = await discoverSkillMarkdownFiles(root)
    const skillIds: number[] = []
    for (const file of files) {
      const markdown = await fs.readFile(file.skillFile, 'utf8')
      skillIds.push(createSkill(parseSkillMarkdown(markdown, file.name)))
    }
    return { ok: true, imported: skillIds.length, skillIds }
  } catch (error) {
    return {
      ok: false,
      imported: 0,
      skillIds: [],
      error: error instanceof Error ? error.message : String(error)
    }
  }
}
