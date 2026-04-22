import { promises as fs } from 'fs'
import { dirname, join } from 'path'

function normalizeRelPath(relPath: string): string {
  return relPath
    .split(/[\\/]+/)
    .map((p) => p.trim())
    .filter((p) => p.length > 0 && p !== '.' && p !== '..')
    .join('/')
}

export function repoMemoryDir(root: string): string {
  return join(root, '.multi-ai-code', 'repo-memory')
}

export function repoMemoryProjectSummaryPath(root: string): string {
  return join(repoMemoryDir(root), 'project-summary.md')
}

export function repoMemoryRecentTopicsPath(root: string): string {
  return join(repoMemoryDir(root), 'recent-topics.json')
}

export function repoMemoryFileNotePath(root: string, relPath: string): string {
  const safeRel = normalizeRelPath(relPath)
  return join(repoMemoryDir(root), 'file-notes', `${safeRel}.md`)
}

export async function ensureRepoMemoryExcluded(root: string): Promise<void> {
  const excludePath = join(root, '.git', 'info', 'exclude')
  await fs.mkdir(dirname(excludePath), { recursive: true })
  const line = '.multi-ai-code/repo-memory/'
  let text = ''
  try {
    text = await fs.readFile(excludePath, 'utf8')
  } catch {
    /* ignore */
  }
  const existingLines = text
    .split(/\r?\n/)
    .map((x) => x.trim())
    .filter(Boolean)
  if (existingLines.includes(line)) return
  const next = existingLines.length > 0 ? `${existingLines.join('\n')}\n${line}\n` : `${line}\n`
  await fs.writeFile(excludePath, next, 'utf8')
}

export interface RepoMemoryTopic {
  at: string
  filePath: string
  topic: string
}

export async function readRepoMemory(root: string): Promise<{
  summary: string
  recentTopics: RepoMemoryTopic[]
}> {
  await ensureRepoMemoryExcluded(root)
  const [summary, recentRaw] = await Promise.all([
    fs.readFile(repoMemoryProjectSummaryPath(root), 'utf8').catch(() => ''),
    fs.readFile(repoMemoryRecentTopicsPath(root), 'utf8').catch(() => '[]')
  ])
  let recentTopics: RepoMemoryTopic[] = []
  try {
    const parsed = JSON.parse(recentRaw)
    if (Array.isArray(parsed)) {
      recentTopics = parsed.filter((x) => x && typeof x === 'object') as RepoMemoryTopic[]
    }
  } catch {
    recentTopics = []
  }
  return { summary, recentTopics }
}

export async function readRepoFileNote(root: string, relPath: string): Promise<string> {
  return fs.readFile(repoMemoryFileNotePath(root, relPath), 'utf8').catch(() => '')
}

function keepTail(text: string, maxChars: number): string {
  if (text.length <= maxChars) return text
  return text.slice(text.length - maxChars)
}

function summarizeTopic(text: string): string {
  const first = text
    .split(/\r?\n/)
    .map((x) => x.trim())
    .find(Boolean) ?? '本次代码分析更新'
  return first.slice(0, 140)
}

export async function applyRepoMemoryUpdate(input: {
  root: string
  filePath: string
  memoryUpdate: string
}): Promise<{
  summary: string
  fileNote: string
  recentTopics: RepoMemoryTopic[]
}> {
  await ensureRepoMemoryExcluded(input.root)
  const memoryText = input.memoryUpdate.trim()
  const memoryRoot = repoMemoryDir(input.root)
  const summaryPath = repoMemoryProjectSummaryPath(input.root)
  const fileNotePath = repoMemoryFileNotePath(input.root, input.filePath)
  const recentPath = repoMemoryRecentTopicsPath(input.root)
  await fs.mkdir(dirname(summaryPath), { recursive: true })
  await fs.mkdir(dirname(fileNotePath), { recursive: true })
  await fs.mkdir(dirname(recentPath), { recursive: true })

  const current = await readRepoMemory(input.root)
  const currentFileNote = await readRepoFileNote(input.root, input.filePath)
  if (!memoryText) {
    return {
      summary: current.summary,
      fileNote: currentFileNote,
      recentTopics: current.recentTopics
    }
  }

  const stamp = new Date().toISOString()
  const entryHead = `## ${stamp} · ${input.filePath}`
  const entryBody = `${entryHead}\n${memoryText}\n`
  const summary = keepTail(
    [current.summary.trim(), entryBody].filter(Boolean).join('\n\n').trim() + '\n',
    12000
  )
  const fileNote = keepTail(
    [currentFileNote.trim(), entryBody].filter(Boolean).join('\n\n').trim() + '\n',
    12000
  )
  const topic: RepoMemoryTopic = {
    at: stamp,
    filePath: input.filePath,
    topic: summarizeTopic(memoryText)
  }
  const recentTopics = [topic, ...current.recentTopics]
    .slice(0, 40)
    .map((x) => ({ at: x.at, filePath: x.filePath, topic: x.topic }))

  await Promise.all([
    fs.writeFile(summaryPath, summary, 'utf8'),
    fs.writeFile(fileNotePath, fileNote, 'utf8'),
    fs.writeFile(recentPath, JSON.stringify(recentTopics, null, 2), 'utf8')
  ])

  return { summary, fileNote, recentTopics }
}
