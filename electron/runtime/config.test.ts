import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  DEFAULT_RUNTIME_CONFIG,
  getProjectRuntimeConfig,
  normalizeRuntimeConfig,
  setProjectRuntimeConfig,
  type ProjectRuntimeConfig,
} from './config.js'

let root: string
let projectDir: string
let metaPath: string

beforeEach(async () => {
  root = await fs.mkdtemp(join(tmpdir(), 'runtime-config-'))
  projectDir = join(root, 'project')
  metaPath = join(projectDir, 'project.json')
  await fs.mkdir(projectDir, { recursive: true })
})

afterEach(async () => {
  await fs.rm(root, { recursive: true, force: true })
})

describe('normalizeRuntimeConfig', () => {
  it('returns the default config for invalid input', () => {
    expect(normalizeRuntimeConfig(null)).toEqual<ProjectRuntimeConfig>(DEFAULT_RUNTIME_CONFIG)
    expect(normalizeRuntimeConfig({ command: 42 })).toEqual<ProjectRuntimeConfig>(
      DEFAULT_RUNTIME_CONFIG
    )
  })

  it('trims fields and defaults environment settings', () => {
    expect(
      normalizeRuntimeConfig({
        enabled: true,
        cwd: ' app ',
        command: ' npm run dev ',
        envType: 'visual-studio',
      })
    ).toEqual<ProjectRuntimeConfig>({
      enabled: true,
      cwd: 'app',
      command: 'npm run dev',
      envType: 'visual-studio',
      visualStudioInstanceId: '',
      outputEncoding: 'auto',
    })
  })
})

describe('getProjectRuntimeConfig', () => {
  it('returns default config when runtime_config is absent', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p1' }, null, 2), 'utf8')

    const result = await getProjectRuntimeConfig(metaPath)

    expect(result).toEqual({ ok: true, value: DEFAULT_RUNTIME_CONFIG })
  })

  it('repairs corrupted project.json and reads runtime_config', async () => {
    await fs.writeFile(
      metaPath,
      '{ "id": "p2", "runtime_config": { "enabled": true, "cwd": ".", "command": "npm run dev" } } trailing',
      'utf8'
    )

    const result = await getProjectRuntimeConfig(metaPath)

    expect(result).toEqual({
      ok: true,
      repaired: true,
      value: {
        enabled: true,
        cwd: '.',
        command: 'npm run dev',
        envType: 'msys',
        visualStudioInstanceId: '',
        outputEncoding: 'auto',
      },
    })
  })
})

describe('setProjectRuntimeConfig', () => {
  it('allows saving the disabled default config without a command', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p-default' }, null, 2), 'utf8')

    const result = await setProjectRuntimeConfig(metaPath, DEFAULT_RUNTIME_CONFIG)

    expect(result).toEqual({ ok: true })
    const saved = JSON.parse(await fs.readFile(metaPath, 'utf8'))
    expect(saved.runtime_config).toEqual(DEFAULT_RUNTIME_CONFIG)
  })

  it('rejects absolute and parent-traversal working directories', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p3' }, null, 2), 'utf8')

    const absoluteResult = await setProjectRuntimeConfig(metaPath, {
      enabled: true,
      cwd: 'C:\\outside',
      command: 'npm run dev',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'auto',
    })
    expect(absoluteResult.ok).toBe(false)
    if (absoluteResult.ok) throw new Error('expected validation failure')
    expect(absoluteResult.details?.[0].path).toBe('runtime_config.cwd')

    const traversalResult = await setProjectRuntimeConfig(metaPath, {
      enabled: true,
      cwd: '..\\outside',
      command: 'npm run dev',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'auto',
    })
    expect(traversalResult.ok).toBe(false)
    if (traversalResult.ok) throw new Error('expected validation failure')
    expect(traversalResult.details?.[0].message).toContain('parent traversal')
  })

  it('rejects empty command and missing Visual Studio instance', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p4' }, null, 2), 'utf8')

    const result = await setProjectRuntimeConfig(metaPath, {
      enabled: true,
      cwd: '.',
      command: '',
      envType: 'visual-studio',
      visualStudioInstanceId: '',
      outputEncoding: 'auto',
    })

    expect(result.ok).toBe(false)
    if (result.ok) throw new Error('expected validation failure')
    expect(result.details?.map((issue) => issue.path)).toEqual([
      'runtime_config.visualStudioInstanceId',
      'runtime_config.command',
    ])
  })

  it('persists normalized runtime_config without dropping other metadata', async () => {
    await fs.writeFile(
      metaPath,
      JSON.stringify(
        { id: 'p5', name: 'Demo', build_config: { enabled: false, steps: [] } },
        null,
        2
      ),
      'utf8'
    )

    const result = await setProjectRuntimeConfig(metaPath, {
      enabled: true,
      cwd: ' app ',
      command: ' npm run dev ',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'utf8',
    })

    expect(result).toEqual({ ok: true })
    const saved = JSON.parse(await fs.readFile(metaPath, 'utf8'))
    expect(saved.name).toBe('Demo')
    expect(saved.build_config).toEqual({ enabled: false, steps: [] })
    expect(saved.runtime_config).toEqual({
      enabled: true,
      cwd: 'app',
      command: 'npm run dev',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'utf8',
    })
  })
})
