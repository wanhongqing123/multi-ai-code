import { execFile } from 'node:child_process'
import { createHash } from 'node:crypto'
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
  it('lists every changed file in a clickable index anchored to its section', async () => {
    const { repo, reports } = await createWorkspace()
    await fs.writeFile(join(repo, 'README.md'), '# Demo\n')
    await commitAll(repo)
    // 三个文件足够暴露「索引只列了第一个」和「锚点没生成」这两类问题；
    // 一个文件的用例对它们完全不敏感。
    for (const name of ['alpha.ts', 'beta.ts', 'gamma.ts']) {
      await fs.writeFile(join(repo, name), `export const ${name.split('.')[0]} = 1\n`)
    }

    const result = await createGitDiffReport({ targetRepo: repo, outputDir: reports })

    expect(result.ok).toBe(true)
    if (!result.ok || !result.attachmentPath) throw new Error('expected an attachment')
    const html = await fs.readFile(result.attachmentPath, 'utf8')

    // 每个文件都要有一条索引项，并且指向一个真实存在的锚点。
    for (const name of ['alpha.ts', 'beta.ts', 'gamma.ts']) {
      expect(html).toContain(name)
    }
    const links = [...html.matchAll(/<li><a href="#(f\d+)">/g)].map((m) => m[1])
    expect(links.length).toBe(3)
    for (const id of links) {
      // 索引项指向的锚点必须真的在文档里，否则点了不动——而且不报错。
      expect(html).toContain(`<a name="${id}"></a>`)
    }
    expect(html).toContain('变更文件（3）')
  })

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

  it('renders a resolved commit as responsive split and unified HTML', async () => {
    const { repo, reports } = await createWorkspace()
    await fs.writeFile(join(repo, 'app.ts'), 'export const value = 1\n')
    await commitAll(repo)
    const base = await git(repo, ['rev-parse', 'HEAD'])
    await fs.writeFile(join(repo, 'app.ts'), 'export const value = "<two>"\n')
    await commitAll(repo, 'feature')
    const head = await git(repo, ['rev-parse', 'HEAD'])
    await fs.writeFile(join(repo, 'app.ts'), 'working tree must not leak\n')

    const result = await createGitDiffReport({
      targetRepo: repo,
      args: `--commit ${head}`,
      outputDir: reports
    })

    expect(result.ok, JSON.stringify(result)).toBe(true)
    if (!result.ok || !result.attachmentPath || !result.artifact) return
    expect(result.attachmentPath).toMatch(/\.html$/)
    expect(result.artifact.source).toMatchObject({
      kind: 'commit',
      requestedRef: head,
      baseOid: base,
      headOid: head
    })
    const report = await fs.readFile(result.attachmentPath, 'utf8')
    expect(report).toContain('class="split"')
    expect(report).toContain('class="unified"')
    const split = report.split('<!-- MAICHAT_SPLIT_START -->')[1]?.split('<!-- MAICHAT_SPLIT_END -->')[0]
    expect(split).toContain('<table class="split-table"')
    expect(split).not.toContain('<pre>')
    expect(report.match(/class="qt-separator"/g)).toHaveLength(2)
    expect(report.match(/> · <\/span>/g)).toHaveLength(2)
    expect(report).toContain('&quot;&lt;two&gt;&quot;')
    expect(report).not.toContain('working tree must not leak')
    expect(result.artifact.sha256).toBe(
      createHash('sha256').update(report, 'utf8').digest('hex')
    )
    expect(result.attachmentPath).toContain(result.artifact.sha256)
    expect(result.artifact.sizeBytes).toBe(Buffer.byteLength(report, 'utf8'))
  })

  it('resolves an explicit commit range and rejects option-shaped refs', async () => {
    const { repo, reports } = await createWorkspace()
    await fs.writeFile(join(repo, 'range.txt'), 'before\n')
    await commitAll(repo)
    const base = await git(repo, ['rev-parse', 'HEAD'])
    await fs.writeFile(join(repo, 'range.txt'), 'after\n')
    await commitAll(repo, 'after')
    const head = await git(repo, ['rev-parse', 'HEAD'])

    const result = await createGitDiffReport({
      targetRepo: repo,
      args: `--range ${base}..${head}`,
      outputDir: reports
    })
    expect(result.ok, JSON.stringify(result)).toBe(true)
    if (!result.ok || !result.artifact) return
    expect(result.artifact.source).toMatchObject({ kind: 'range', baseOid: base, headOid: head })

    const invalid = await createGitDiffReport({
      targetRepo: repo,
      args: '--commit --help',
      outputDir: reports
    })
    expect(invalid.ok).toBe(false)
    expect(invalid.text).toContain('不能以 - 开头')
  })
})
