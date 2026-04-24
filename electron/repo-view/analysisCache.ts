import { promises as fs } from 'fs'
import { join } from 'path'

const RULE = 'repo-view/analyses/'

export async function ensureAnalysisCacheDir(repoRoot: string): Promise<void> {
  const baseDir = join(repoRoot, '.multi-ai-code')
  const cacheDir = join(baseDir, 'repo-view', 'analyses')
  await fs.mkdir(cacheDir, { recursive: true })

  const giPath = join(baseDir, '.gitignore')
  let current = ''
  try {
    current = await fs.readFile(giPath, 'utf8')
  } catch {
    /* missing — treat as empty */
  }

  const lines = current.split('\n').map((l) => l.trim())
  if (lines.includes(RULE)) return

  const next =
    current.length === 0 || current.endsWith('\n')
      ? `${current}${RULE}\n`
      : `${current}\n${RULE}\n`
  await fs.writeFile(giPath, next, 'utf8')
}
