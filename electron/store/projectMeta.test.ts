import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  readProjectMetaFile,
  writeProjectMetaFile,
  type ProjectMetaReadResult,
} from './projectMeta.js'

let root: string
let projectDir: string
let metaPath: string

beforeEach(async () => {
  root = await fs.mkdtemp(join(tmpdir(), 'project-meta-'))
  projectDir = join(root, 'project')
  metaPath = join(projectDir, 'project.json')
  await fs.mkdir(projectDir, { recursive: true })
})

afterEach(async () => {
  await fs.rm(root, { recursive: true, force: true })
})

describe('readProjectMetaFile', () => {
  it('reads valid project.json without repair', async () => {
    await fs.writeFile(
      metaPath,
      JSON.stringify({ id: 'p1', name: 'demo', target_repo: 'C:/repo' }, null, 2),
      'utf8'
    )

    const result = await readProjectMetaFile(metaPath)

    expect(result).toEqual<ProjectMetaReadResult>({
      ok: true,
      repaired: false,
      meta: { id: 'p1', name: 'demo', target_repo: 'C:/repo' },
    })
    const names = await fs.readdir(projectDir)
    expect(names.some((name) => name.startsWith('project.json.autofix-'))).toBe(false)
  })

  it('repairs trailing garbage after the first complete top-level object', async () => {
    await fs.writeFile(
      metaPath,
      '{\n  "id": "p2",\n  "name": "demo",\n  "repo_view_ai_settings": {\n    "ai_cli": "codex",\n    "env": {\n      "FOO": "bar"\n    }\n  }\n}  "ai_settings": { "ai_cli": "claude" }\n',
      'utf8'
    )

    const result = await readProjectMetaFile(metaPath)

    if (!result.ok) {
      throw new Error('expected recoverable project meta')
    }
    expect(result).toEqual({
      ok: true,
      repaired: true,
      meta: {
        id: 'p2',
        name: 'demo',
        repo_view_ai_settings: {
          ai_cli: 'codex',
          env: {
            FOO: 'bar',
          },
        },
      },
    })
    await expect(fs.readFile(metaPath, 'utf8')).resolves.toBe(
      '{\n' +
        '  "id": "p2",\n' +
        '  "name": "demo",\n' +
        '  "repo_view_ai_settings": {\n' +
        '    "ai_cli": "codex",\n' +
        '    "env": {\n' +
        '      "FOO": "bar"\n' +
        '    }\n' +
        '  }\n' +
        '}'
    )

    const names = await fs.readdir(projectDir)
    const backup = names.find((name) => name.startsWith('project.json.autofix-') && name.endsWith('.bak'))
    expect(backup).toBeTruthy()
    await expect(fs.readFile(join(projectDir, backup!), 'utf8')).resolves.toBe(
      '{\n  "id": "p2",\n  "name": "demo",\n  "repo_view_ai_settings": {\n    "ai_cli": "codex",\n    "env": {\n      "FOO": "bar"\n    }\n  }\n}  "ai_settings": { "ai_cli": "claude" }\n'
    )

    await writeProjectMetaFile(metaPath, {
      ...result.meta,
      ai_settings: { ai_cli: 'claude', command: 'claude' },
    })

    await expect(fs.readFile(metaPath, 'utf8')).resolves.toBe(
      '{\n' +
        '  "id": "p2",\n' +
        '  "name": "demo",\n' +
        '  "repo_view_ai_settings": {\n' +
        '    "ai_cli": "codex",\n' +
        '    "env": {\n' +
        '      "FOO": "bar"\n' +
        '    }\n' +
        '  },\n' +
        '  "ai_settings": {\n' +
        '    "ai_cli": "claude",\n' +
        '    "command": "claude"\n' +
        '  }\n' +
        '}'
    )
  })

  it('prefers the last complete top-level object when multiple objects were concatenated', async () => {
    await fs.writeFile(
      metaPath,
      '{\n  "id": "stale",\n  "ai_settings": {\n    "ai_cli": "claude"\n  }\n}{\n  "id": "fresh",\n  "ai_settings": {\n    "ai_cli": "codex"\n  }\n}\n',
      'utf8'
    )

    const result = await readProjectMetaFile(metaPath)

    expect(result).toEqual({
      ok: true,
      repaired: true,
      meta: {
        id: 'fresh',
        ai_settings: {
          ai_cli: 'codex',
        },
      },
    })
    await expect(fs.readFile(metaPath, 'utf8')).resolves.toBe(
      '{\n' +
        '  "id": "fresh",\n' +
        '  "ai_settings": {\n' +
        '    "ai_cli": "codex"\n' +
        '  }\n' +
        '}'
    )
  })

  it('returns a structured unrecoverable error for broken JSON', async () => {
    await fs.writeFile(metaPath, '{\n  "id": "p3",\n  "name": \n', 'utf8')

    const result = await readProjectMetaFile(metaPath)

    expect(result).toEqual({
      ok: false,
      repaired: false,
      error: 'project settings corrupted and unrecoverable',
    })
  })
})

describe('writeProjectMetaFile', () => {
  it('writes stable two-space formatted JSON', async () => {
    await writeProjectMetaFile(metaPath, {
      id: 'p4',
      name: 'demo',
      ai_settings: { ai_cli: 'claude', env: {} },
    })

    await expect(fs.readFile(metaPath, 'utf8')).resolves.toBe(
      '{\n' +
        '  "id": "p4",\n' +
        '  "name": "demo",\n' +
        '  "ai_settings": {\n' +
        '    "ai_cli": "claude",\n' +
        '    "env": {}\n' +
        '  }\n' +
        '}'
    )
  })
})
