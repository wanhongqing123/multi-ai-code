import { resolve } from 'path'

export function normalizePathForCompare(path: string): string {
  const normalized = resolve(path).replace(/[\\/]+$/, '')
  return process.platform === 'win32' ? normalized.toLowerCase() : normalized
}
