import { afterEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { parseGitLog, recentlyChangedFiles } from './sources.js'

describe('parseGitLog', () => {
  it('parses the pipe-delimited git log format', () => {
    const out = parseGitLog(
      'a1b2c3d|1700000000|Alice|fix: tighten cache key\n' +
        'd4e5f6g|1699900000|Bob|feat: add login route\n'
    )
    expect(out).toHaveLength(2)
    expect(out[0]).toEqual({
      hash: 'a1b2c3d',
      ts: 1700000000,
      author: 'Alice',
      subject: 'fix: tighten cache key'
    })
  })

  it('preserves pipes that appear inside the commit subject', () => {
    const out = parseGitLog('a|1700|me|feat: A|B work')
    expect(out[0].subject).toBe('feat: A|B work')
  })

  it('returns an empty array for empty input', () => {
    expect(parseGitLog('')).toEqual([])
  })

  it('skips malformed lines', () => {
    const out = parseGitLog('not-enough-pipes\n' + 'a|1700|me|good one\n' + '|||\n')
    expect(out).toHaveLength(1)
    expect(out[0].hash).toBe('a')
  })

  it('skips lines with non-numeric timestamps', () => {
    const out = parseGitLog('a|not-a-number|me|subject\n' + 'b|1700|me|ok\n')
    expect(out).toHaveLength(1)
    expect(out[0].hash).toBe('b')
  })
})

describe('recentlyChangedFiles', () => {
  const tempRoots: string[] = []

  afterEach(async () => {
    while (tempRoots.length) {
      const r = tempRoots.pop()
      if (r) await fs.rm(r, { recursive: true, force: true })
    }
  })

  async function mkRepo(): Promise<string> {
    const r = await fs.mkdtemp(join(tmpdir(), 'kb-src-'))
    tempRoots.push(r)
    return r
  }

  async function touch(repo: string, rel: string, ageDays: number): Promise<void> {
    const abs = join(repo, rel)
    await fs.mkdir(join(abs, '..'), { recursive: true })
    await fs.writeFile(abs, 'content', 'utf8')
    const mtime = Date.now() - ageDays * 24 * 60 * 60 * 1000
    await fs.utimes(abs, mtime / 1000, mtime / 1000)
  }

  it('returns files modified within the window, sorted newest first', async () => {
    const r = await mkRepo()
    await touch(r, 'src/a.ts', 1)
    await touch(r, 'src/b.ts', 5)
    await touch(r, 'src/old.ts', 100)
    const got = await recentlyChangedFiles(r, 30, 50)
    const paths = got.map((g) => g.relPath)
    expect(paths).toContain('src/a.ts')
    expect(paths).toContain('src/b.ts')
    expect(paths).not.toContain('src/old.ts')
    expect(paths.indexOf('src/a.ts')).toBeLessThan(paths.indexOf('src/b.ts'))
  })

  it('skips node_modules / dist / build / .git', async () => {
    const r = await mkRepo()
    await touch(r, 'src/keep.ts', 1)
    await touch(r, 'node_modules/foo/index.js', 1)
    await touch(r, 'dist/bundle.js', 1)
    await touch(r, 'build/out.js', 1)
    await touch(r, '.git/HEAD', 1)
    const got = await recentlyChangedFiles(r, 30, 50)
    const paths = got.map((g) => g.relPath)
    expect(paths).toEqual(['src/keep.ts'])
  })

  it('skips dotfile directories at any depth', async () => {
    const r = await mkRepo()
    await touch(r, 'src/.hidden/secret.ts', 1)
    await touch(r, 'src/visible.ts', 1)
    const got = await recentlyChangedFiles(r, 30, 50)
    expect(got.map((g) => g.relPath)).toEqual(['src/visible.ts'])
  })

  it('falls back to walking the repo root when no known source roots exist', async () => {
    const r = await mkRepo()
    await touch(r, 'main.go', 1)
    await touch(r, 'internal/foo.go', 1)
    const got = await recentlyChangedFiles(r, 30, 50)
    const paths = got.map((g) => g.relPath)
    expect(paths.sort()).toEqual(['internal/foo.go', 'main.go'])
  })

  it('respects the limit argument', async () => {
    const r = await mkRepo()
    for (let i = 0; i < 10; i++) {
      await touch(r, `src/f${i}.ts`, i)
    }
    const got = await recentlyChangedFiles(r, 30, 3)
    expect(got).toHaveLength(3)
  })
})
