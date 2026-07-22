import { execFile } from 'node:child_process'
import { promises as fs } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { promisify } from 'node:util'
import { afterEach, describe, expect, it } from 'vitest'
import { createGitDiffReport } from '../../../electron/remote-im/gitDiffReport.js'

const execFileAsync = promisify(execFile)
const cleanupPaths: string[] = []

async function git(cwd: string, args: string[]): Promise<string> {
  const result = await execFileAsync('git', args, { cwd, encoding: 'utf8' })
  return result.stdout.trim()
}

async function createRepository(parent: string, name = 'repo'): Promise<string> {
  const repo = join(parent, name)
  await fs.mkdir(repo, { recursive: true })
  await git(repo, ['init'])
  await git(repo, ['config', 'user.name', 'Remote IM Test'])
  await git(repo, ['config', 'user.email', 'remote-im@example.test'])
  return repo
}

async function commitAll(repo: string, message = 'initial'): Promise<void> {
  await git(repo, ['add', '-A'])
  await git(repo, ['commit', '-m', message])
}

async function createWorkspace(): Promise<{ root: string; repo: string; reports: string }> {
  const root = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-diff-test-'))
  cleanupPaths.push(root)
  const repo = await createRepository(root)
  return { root, repo, reports: join(root, 'reports') }
}

afterEach(async () => {
  await Promise.all(cleanupPaths.splice(0).map((path) => fs.rm(path, { recursive: true, force: true })))
})

describe('Git Diff report', () => {
  it('reports a clean repository without creating an attachment', async () => {
    const { repo, reports } = await createWorkspace()
    await fs.writeFile(join(repo, 'README.md'), '# Demo\n')
    await commitAll(repo)

    const result = await createGitDiffReport({ targetRepo: repo, outputDir: reports })

    expect(result.ok).toBe(true)
    if (!result.ok) return
    expect(result.text).toContain('没有未提交改动')
    expect(result.attachmentPath).toBeUndefined()
  })

  it('combines staged, unstaged and untracked changes while hiding sensitive content', async () => {
    const { repo, reports } = await createWorkspace()
    await fs.mkdir(join(repo, 'src'))
    await fs.writeFile(join(repo, 'src/app.ts'), 'export const value = 1\n')
    await fs.writeFile(join(repo, '.env'), 'API_TOKEN=old-secret\n')
    await commitAll(repo)

    await fs.writeFile(join(repo, 'src/app.ts'), 'export const value = 2\n')
    await git(repo, ['add', 'src/app.ts'])
    await fs.writeFile(join(repo, 'src/app.ts'), 'export const value = 3\n')
    await fs.writeFile(join(repo, 'notes.md'), 'new note\n')
    await fs.writeFile(join(repo, '.env'), 'API_TOKEN=do-not-send\n')

    const result = await createGitDiffReport({ targetRepo: repo, outputDir: reports })

    expect(result.ok).toBe(true)
    if (!result.ok || !result.attachmentPath) return
    expect(result.text).toContain('3 个未提交文件')
    expect(result.text).toContain('.env')
    expect(result.text).toContain('内容已隐藏')
    const report = await fs.readFile(result.attachmentPath, 'utf8')
    expect(report).toContain('export const value = 3')
    expect(report).not.toContain('export const value = 2')
    expect(report).toContain('new note')
    expect(report).not.toContain('do-not-send')
    expect(report).not.toContain('old-secret')
  })

  it('supports stat-only and repository-confined path filters', async () => {
    const { repo, reports } = await createWorkspace()
    await fs.mkdir(join(repo, 'src'))
    await fs.writeFile(join(repo, 'src/app.ts'), 'one\n')
    await fs.writeFile(join(repo, 'other.txt'), 'one\n')
    await commitAll(repo)
    await fs.writeFile(join(repo, 'src/app.ts'), 'two\n')
    await fs.writeFile(join(repo, 'other.txt'), 'two\n')

    const stat = await createGitDiffReport({
      targetRepo: repo,
      args: '--stat src',
      outputDir: reports
    })
    expect(stat.ok, JSON.stringify(stat)).toBe(true)
    if (!stat.ok) return
    expect(stat.text).toContain('src/app.ts')
    expect(stat.text).not.toContain('other.txt')
    expect(stat.attachmentPath).toBeUndefined()

    const outside = await createGitDiffReport({
      targetRepo: repo,
      args: '../outside.txt',
      outputDir: reports
    })
    expect(outside.ok).toBe(false)
    expect(outside.text).toContain('当前仓库内')
  })

  it('recursively includes initialized submodule working tree changes', async () => {
    const { root, repo, reports } = await createWorkspace()
    const source = await createRepository(root, 'child-source')
    await fs.writeFile(join(source, 'child.txt'), 'before\n')
    await commitAll(source)

    await fs.writeFile(join(repo, 'README.md'), '# Parent\n')
    await commitAll(repo)
    await git(repo, [
      '-c',
      'protocol.file.allow=always',
      'submodule',
      'add',
      source,
      'deps/child'
    ])
    await commitAll(repo, 'add child')
    await fs.writeFile(join(repo, 'deps/child/child.txt'), 'after\n')
    await fs.writeFile(join(repo, 'deps/child/new.txt'), 'new child file\n')

    const result = await createGitDiffReport({ targetRepo: repo, outputDir: reports })

    expect(result.ok).toBe(true)
    if (!result.ok || !result.attachmentPath) return
    expect(result.text).toContain('deps/child/child.txt')
    expect(result.text).toContain('deps/child/new.txt')
    const report = await fs.readFile(result.attachmentPath, 'utf8')
    expect(report).toContain('Submodule: deps/child')
    expect(report).toContain('after')
    expect(report).toContain('new child file')
  })
})
