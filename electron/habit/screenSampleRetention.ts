/**
 * Disk-side retention sweep for the screen-sampler thumbnails written by
 * `screenSampler.ts`.
 *
 * Two responsibilities:
 *   1. Delete `.png` files older than `retentionDays`.
 *   2. Enforce a hard total-disk cap. If the entire directory exceeds
 *      `hardCapBytes`, prune oldest files until under `softCapBytes`.
 *
 * The functions are deliberately decoupled from habit_events DB rows —
 * we operate purely on the filesystem so a corrupt DB doesn't strand
 * orphan files (and conversely, an orphaned DB row will have its file
 * already gone). The DB-side retention sweep in habit/scheduler.ts is
 * unchanged.
 */

import { promises as fs } from 'fs'
import { join } from 'path'

export const SCREEN_SAMPLE_HARD_CAP_BYTES = 1 * 1024 * 1024 * 1024 // 1 GB
export const SCREEN_SAMPLE_SOFT_CAP_BYTES = 800 * 1024 * 1024     // 800 MB

export interface FileEntry {
  path: string
  size: number
  mtimeMs: number
}

/**
 * Walks `<root>/YYYY-MM-DD/*.png`. Tolerant of missing root (returns []).
 * Skips anything that isn't a `.png` file.
 */
export async function listSampleFiles(root: string): Promise<FileEntry[]> {
  let dayDirs: import('fs').Dirent[]
  try {
    dayDirs = await fs.readdir(root, { withFileTypes: true })
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === 'ENOENT') return []
    throw err
  }

  const out: FileEntry[] = []
  for (const d of dayDirs) {
    if (!d.isDirectory()) continue
    // Daily folders are date-strings; skip anything that doesn't look like
    // YYYY-MM-DD so unrelated junk in the root never gets walked.
    if (!/^\d{4}-\d{2}-\d{2}$/.test(d.name)) continue
    const dayPath = join(root, d.name)
    let files: import('fs').Dirent[]
    try {
      files = await fs.readdir(dayPath, { withFileTypes: true })
    } catch {
      continue
    }
    for (const f of files) {
      if (!f.isFile()) continue
      if (!f.name.endsWith('.png') && !f.name.endsWith('.webp')) continue
      const full = join(dayPath, f.name)
      try {
        const st = await fs.stat(full)
        out.push({ path: full, size: st.size, mtimeMs: st.mtimeMs })
      } catch {
        /* file vanished between readdir and stat — ignore */
      }
    }
  }
  return out
}

/**
 * Pure planner: given the file list + thresholds, decide what to delete.
 *
 *   - Files older than `cutoffMs` go into `expired`.
 *   - If the remaining total still exceeds `hardCapBytes`, the oldest
 *     files (by mtime) go into `evicted` until total ≤ `softCapBytes`.
 *
 * Exported separately so tests can drive it without touching disk.
 */
export function planRetentionSweep(
  files: FileEntry[],
  cutoffMs: number,
  thresholds: {
    hardCapBytes: number
    softCapBytes: number
  } = {
    hardCapBytes: SCREEN_SAMPLE_HARD_CAP_BYTES,
    softCapBytes: SCREEN_SAMPLE_SOFT_CAP_BYTES
  }
): { expired: FileEntry[]; evicted: FileEntry[]; finalBytes: number } {
  const expired: FileEntry[] = []
  const surviving: FileEntry[] = []
  for (const f of files) {
    if (f.mtimeMs < cutoffMs) expired.push(f)
    else surviving.push(f)
  }
  let total = surviving.reduce((s, f) => s + f.size, 0)
  const evicted: FileEntry[] = []
  if (total > thresholds.hardCapBytes) {
    surviving.sort((a, b) => a.mtimeMs - b.mtimeMs)
    while (total > thresholds.softCapBytes && surviving.length > 0) {
      const oldest = surviving.shift()!
      evicted.push(oldest)
      total -= oldest.size
    }
  }
  return { expired, evicted, finalBytes: total }
}

export interface SweepResult {
  expiredDeleted: number
  evictedDeleted: number
  bytesAfter: number
  errors: number
}

/**
 * Apply the planned deletions. Per-file failures are counted but never
 * thrown — the sweep is best-effort.
 */
export async function applyRetentionSweep(
  root: string,
  retentionDays: number,
  now: number = Date.now(),
  thresholds = {
    hardCapBytes: SCREEN_SAMPLE_HARD_CAP_BYTES,
    softCapBytes: SCREEN_SAMPLE_SOFT_CAP_BYTES
  }
): Promise<SweepResult> {
  const files = await listSampleFiles(root)
  const cutoff = now - Math.max(0, retentionDays) * 24 * 60 * 60 * 1000
  const plan = planRetentionSweep(files, cutoff, thresholds)
  let errors = 0
  for (const f of [...plan.expired, ...plan.evicted]) {
    try {
      await fs.unlink(f.path)
    } catch {
      errors++
    }
  }
  // Drop empty day folders so the directory doesn't accumulate corpses.
  try {
    const remainingDirs = await fs.readdir(root, { withFileTypes: true })
    for (const d of remainingDirs) {
      if (!d.isDirectory()) continue
      if (!/^\d{4}-\d{2}-\d{2}$/.test(d.name)) continue
      const sub = join(root, d.name)
      try {
        const left = await fs.readdir(sub)
        if (left.length === 0) await fs.rmdir(sub)
      } catch {
        /* tolerate */
      }
    }
  } catch {
    /* root itself may have been removed mid-sweep — ignore */
  }
  return {
    expiredDeleted: plan.expired.length,
    evictedDeleted: plan.evicted.length,
    bytesAfter: plan.finalBytes,
    errors
  }
}
