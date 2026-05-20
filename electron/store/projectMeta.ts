import { promises as fs } from 'fs'
import { dirname, join } from 'path'

export type ProjectMeta = Record<string, unknown>

export type ProjectMetaReadResult =
  | { ok: true; repaired: boolean; meta: ProjectMeta }
  | { ok: false; repaired: false; error: 'project settings corrupted and unrecoverable' }

function parseLastTopLevelObject(raw: string): ProjectMeta | null {
  const first = raw.search(/\S/)
  if (first === -1 || raw[first] !== '{') return null

  let candidateStart: number | null = null
  let depth = 0
  let inString = false
  let escaped = false
  let parsed: ProjectMeta | null = null
  let sawTopLevelNoise = false

  for (let i = first; i < raw.length; i += 1) {
    const ch = raw[i]

    if (inString) {
      if (escaped) {
        escaped = false
      } else if (ch === '\\') {
        escaped = true
      } else if (ch === '"') {
        inString = false
      }
      continue
    }

    if (ch === '"') {
      inString = true
      if (depth === 0 && candidateStart === null) {
        sawTopLevelNoise = true
      }
      continue
    }

    if (ch === '{') {
      if (depth === 0 && !sawTopLevelNoise) {
        candidateStart = i
      } else if (depth === 0) {
        continue
      }
      depth += 1
      continue
    }

    if (ch === '}') {
      depth -= 1
      if (depth === 0 && candidateStart !== null) {
        const candidate = raw.slice(candidateStart, i + 1)
        try {
          parsed = JSON.parse(candidate) as ProjectMeta
        } catch {
          return null
        }
        candidateStart = null
        sawTopLevelNoise = false
      }
      continue
    }

    if (depth === 0 && !/\s/.test(ch)) {
      sawTopLevelNoise = true
    }
  }

  return parsed
}

async function writeBackup(metaPath: string, raw: string): Promise<void> {
  const stamp = new Date().toISOString().replace(/[:.]/g, '-')
  const backupPath = join(dirname(metaPath), `project.json.autofix-${stamp}.bak`)
  await fs.writeFile(backupPath, raw, 'utf8')
}

export async function readProjectMetaFile(metaPath: string): Promise<ProjectMetaReadResult> {
  const raw = await fs.readFile(metaPath, 'utf8')

  try {
    return { ok: true, repaired: false, meta: JSON.parse(raw) as ProjectMeta }
  } catch {
    const repaired = parseLastTopLevelObject(raw)
    if (!repaired) {
      return {
        ok: false,
        repaired: false,
        error: 'project settings corrupted and unrecoverable',
      }
    }

    await writeBackup(metaPath, raw)
    await writeProjectMetaFile(metaPath, repaired)
    return { ok: true, repaired: true, meta: repaired }
  }
}

export async function writeProjectMetaFile(metaPath: string, meta: ProjectMeta): Promise<void> {
  await fs.writeFile(metaPath, JSON.stringify(meta, null, 2), 'utf8')
}
