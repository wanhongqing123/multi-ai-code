import { promises as fs } from 'fs'
import { join, resolve, sep } from 'path'

export interface RepoTreeEntry {
  name: string
  path: string
  isDirectory: boolean
}

const IGNORED_DIRS = new Set(['.git', 'node_modules', 'dist', 'build', 'out'])
const MAX_PREVIEW_BYTES = 2 * 1024 * 1024

export function shouldIgnoreRepoEntry(name: string, isDirectory: boolean): boolean {
  return isDirectory && IGNORED_DIRS.has(name)
}

export function sortRepoEntries<T extends { name: string; isDirectory: boolean }>(entries: T[]): T[] {
  return [...entries].sort((a, b) => {
    if (a.isDirectory !== b.isDirectory) return a.isDirectory ? -1 : 1
    return a.name.localeCompare(b.name, 'en', { sensitivity: 'base' })
  })
}

function ensureInsideRoot(root: string, relPath: string): string {
  const rootAbs = resolve(root)
  const abs = resolve(root, relPath || '.')
  if (abs !== rootAbs && !abs.startsWith(`${rootAbs}${sep}`)) {
    throw new Error('path is outside repo root')
  }
  return abs
}

export async function listRepoTree(root: string, relDir = ''): Promise<RepoTreeEntry[]> {
  const abs = ensureInsideRoot(root, relDir)
  const dirents = await fs.readdir(abs, { withFileTypes: true })
  const entries = dirents
    .filter((d) => !shouldIgnoreRepoEntry(d.name, d.isDirectory()))
    .map((d) => ({
      name: d.name,
      path: relDir ? join(relDir, d.name).split(sep).join('/') : d.name,
      isDirectory: d.isDirectory()
    }))
  return sortRepoEntries(entries)
}

export async function readRepoTextFile(root: string, relPath: string): Promise<{
  content: string
  byteLength: number
}> {
  if (!relPath.trim()) throw new Error('file path is required')
  const abs = ensureInsideRoot(root, relPath)
  const st = await fs.stat(abs)
  if (!st.isFile()) throw new Error('target is not a file')
  if (st.size > MAX_PREVIEW_BYTES) {
    throw new Error(`file too large to preview (> ${MAX_PREVIEW_BYTES} bytes)`)
  }
  const buf = await fs.readFile(abs)
  if (buf.includes(0)) throw new Error('binary file is not previewable')
  return {
    content: buf.toString('utf8'),
    byteLength: buf.byteLength
  }
}
