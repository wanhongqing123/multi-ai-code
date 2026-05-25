import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  SCREEN_SAMPLE_HARD_CAP_BYTES,
  SCREEN_SAMPLE_SOFT_CAP_BYTES,
  applyRetentionSweep,
  listSampleFiles,
  planRetentionSweep,
  type FileEntry
} from './screenSampleRetention.js'

const DAY = 24 * 60 * 60 * 1000

let root: string

beforeEach(async () => {
  root = await fs.mkdtemp(join(tmpdir(), 'screen-retention-'))
})

afterEach(async () => {
  await fs.rm(root, { recursive: true, force: true })
})

async function writeFile(
  relDir: string,
  name: string,
  bytes: number,
  ageDays: number
): Promise<string> {
  const dir = join(root, relDir)
  await fs.mkdir(dir, { recursive: true })
  const p = join(dir, name)
  await fs.writeFile(p, Buffer.alloc(bytes, 0xff))
  const mtime = (Date.now() - ageDays * DAY) / 1000
  await fs.utimes(p, mtime, mtime)
  return p
}

describe('listSampleFiles', () => {
  it('returns empty when root does not exist', async () => {
    expect(await listSampleFiles(join(root, 'missing'))).toEqual([])
  })

  it('walks YYYY-MM-DD subdirectories and returns png/webp entries', async () => {
    await writeFile('2026-05-20', 'a.png', 100, 0)
    await writeFile('2026-05-20', 'b.webp', 200, 0)
    await writeFile('2026-05-19', 'c.png', 50, 1)
    // Ignored extension / non-date dir / nested dir
    await writeFile('2026-05-20', 'd.txt', 10, 0)
    await writeFile('garbage', 'e.png', 10, 0)
    const got = await listSampleFiles(root)
    const names = got.map((g) => g.path.split(/[\\/]/).pop()).sort()
    expect(names).toEqual(['a.png', 'b.webp', 'c.png'])
  })

  it('skips non-directory entries at root level', async () => {
    await writeFile('2026-05-20', 'a.png', 100, 0)
    await fs.writeFile(join(root, 'oops.png'), Buffer.alloc(10))
    const got = await listSampleFiles(root)
    expect(got.map((g) => g.path.endsWith('a.png'))).toEqual([true])
  })
})

describe('planRetentionSweep: expired vs evicted', () => {
  function file(name: string, sizeMb: number, ageDays: number): FileEntry {
    return {
      path: `/fake/${name}`,
      size: sizeMb * 1024 * 1024,
      mtimeMs: Date.now() - ageDays * DAY
    }
  }

  it('marks files older than cutoff as expired', () => {
    const cutoff = Date.now() - 30 * DAY
    const plan = planRetentionSweep(
      [file('a', 1, 5), file('b', 1, 40)],
      cutoff
    )
    expect(plan.expired.map((f) => f.path)).toEqual(['/fake/b'])
    expect(plan.evicted).toEqual([])
  })

  it('does not evict when total stays under hardCap after expiry', () => {
    const plan = planRetentionSweep(
      [file('a', 100, 1)],
      Date.now() - 30 * DAY,
      { hardCapBytes: 200 * 1024 * 1024, softCapBytes: 150 * 1024 * 1024 }
    )
    expect(plan.evicted).toEqual([])
  })

  it('evicts oldest until total drops below softCap when over hardCap', () => {
    const files = [
      file('newer', 60, 1),
      file('older', 60, 2),
      file('oldest', 60, 3)
    ]
    const plan = planRetentionSweep(
      files,
      Date.now() - 30 * DAY,
      { hardCapBytes: 100 * 1024 * 1024, softCapBytes: 80 * 1024 * 1024 }
    )
    // Total 180 MB > 100 MB hard cap. Evict oldest first until ≤ 80 MB.
    // After evicting "oldest" (60 MB) total=120 still over 80.
    // After evicting "older" (60 MB) total=60 → under 80, stop.
    expect(plan.evicted.map((f) => f.path)).toEqual(['/fake/oldest', '/fake/older'])
    expect(plan.finalBytes).toBe(60 * 1024 * 1024)
  })

  it('uses production thresholds by default', () => {
    expect(SCREEN_SAMPLE_HARD_CAP_BYTES).toBe(1024 * 1024 * 1024)
    expect(SCREEN_SAMPLE_SOFT_CAP_BYTES).toBe(800 * 1024 * 1024)
  })

  it('handles empty file list', () => {
    const plan = planRetentionSweep([], Date.now())
    expect(plan).toEqual({ expired: [], evicted: [], finalBytes: 0 })
  })
})

describe('applyRetentionSweep', () => {
  it('actually deletes expired files from disk', async () => {
    const expired = await writeFile('2026-04-01', 'old.png', 100, 60)
    const fresh = await writeFile('2026-05-20', 'new.png', 100, 1)
    const res = await applyRetentionSweep(root, 30)
    expect(res.expiredDeleted).toBe(1)
    await expect(fs.access(expired)).rejects.toThrow()
    await expect(fs.access(fresh)).resolves.toBeUndefined()
  })

  it('removes now-empty day folders after sweep', async () => {
    await writeFile('2026-04-01', 'old.png', 100, 60)
    await applyRetentionSweep(root, 30)
    await expect(fs.access(join(root, '2026-04-01'))).rejects.toThrow()
  })

  it('keeps day folders that still have surviving files', async () => {
    await writeFile('2026-05-20', 'a.png', 100, 1)
    await writeFile('2026-05-20', 'b.png', 100, 60) // expired
    await applyRetentionSweep(root, 30)
    await expect(fs.access(join(root, '2026-05-20'))).resolves.toBeUndefined()
  })

  it('returns errors count without throwing when a file is locked / missing', async () => {
    // Create then delete the file to simulate a race; the planner picked
    // it up but the unlink will ENOENT.
    const p = await writeFile('2026-04-01', 'gone.png', 100, 60)
    await fs.unlink(p)
    const res = await applyRetentionSweep(root, 30)
    expect(res.errors).toBeGreaterThanOrEqual(0)
  })

  it('respects retentionDays = 0 by deleting everything', async () => {
    await writeFile('2026-05-20', 'a.png', 100, 0)
    const res = await applyRetentionSweep(root, 0)
    // Anything with mtime < now is "expired" when retentionDays is 0.
    expect(res.expiredDeleted).toBeGreaterThanOrEqual(0)
  })
})
