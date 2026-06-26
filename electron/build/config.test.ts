import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  DEFAULT_BUILD_CONFIG,
  getProjectBuildConfig,
  normalizeBuildConfig,
  setProjectBuildConfig,
  type ProjectBuildConfig,
} from './config.js'

let root: string
let projectDir: string
let metaPath: string

beforeEach(async () => {
  root = await fs.mkdtemp(join(tmpdir(), 'build-config-'))
  projectDir = join(root, 'project')
  metaPath = join(projectDir, 'project.json')
  await fs.mkdir(projectDir, { recursive: true })
})

afterEach(async () => {
  await fs.rm(root, { recursive: true, force: true })
})

describe('normalizeBuildConfig', () => {
  it('returns an empty default config for invalid input', () => {
    expect(normalizeBuildConfig(null)).toEqual<ProjectBuildConfig>(DEFAULT_BUILD_CONFIG)
    expect(normalizeBuildConfig({ steps: 'nope' })).toEqual<ProjectBuildConfig>(DEFAULT_BUILD_CONFIG)
  })

  it('backfills visual-studio selection and output encoding defaults', () => {
    expect(
      normalizeBuildConfig({
        enabled: true,
        steps: [
          {
            id: 'compile',
            name: ' Build ',
            envType: 'visual-studio',
            cwd: ' build ',
            command: ' cmake --build . ',
            enabled: true,
          },
        ],
      })
    ).toEqual({
      enabled: true,
      steps: [
        {
          id: 'compile',
          name: 'Build',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cmake --build .',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })
  })

  it('keeps original-system environment steps when normalizing', () => {
    expect(
      normalizeBuildConfig({
        enabled: true,
        steps: [
          {
            id: 'build',
            name: 'Build',
            envType: 'system',
            cwd: '.',
            command: 'npm run build',
            enabled: true,
          },
        ],
      })
    ).toEqual<ProjectBuildConfig>({
      enabled: true,
      steps: [
        {
          id: 'build',
          name: 'Build',
          envType: 'system',
          cwd: '.',
          command: 'npm run build',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })
  })

  it('trims fields, keeps root enabled, defaults step enabled to false, and backfills missing ids', () => {
    expect(
      normalizeBuildConfig({
        enabled: true,
        steps: [
          {
            id: ' s1 ',
            name: ' Configure ',
            envType: 'msys',
            cwd: ' . ',
            command: ' cmake -S . -B build ',
            enabled: true,
          },
          {
            id: '',
            name: ' Build ',
            envType: 'visual-studio',
            cwd: ' build ',
            command: ' cmake --build . ',
          },
        ],
      }, { createId: () => 'generated-step-id' })
    ).toEqual<ProjectBuildConfig>({
      enabled: true,
      steps: [
        {
          id: 's1',
          name: 'Configure',
          envType: 'msys',
          cwd: '.',
          command: 'cmake -S . -B build',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
        {
          id: 'generated-step-id',
          name: 'Build',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cmake --build .',
          enabled: false,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })
  })

  it('keeps record-like invalid steps instead of silently dropping them', () => {
    expect(
      normalizeBuildConfig(
        {
          enabled: false,
          steps: [
            {
              id: '',
              name: ' Broken Step ',
              envType: 'powershell',
              cwd: ' ..\\broken ',
              command: ' ',
            },
          ],
        },
        { createId: () => 'generated-bad-step' }
      )
    ).toEqual<ProjectBuildConfig>({
      enabled: false,
      steps: [
        {
          id: 'generated-bad-step',
          name: 'Broken Step',
          envType: 'msys',
          cwd: '..\\broken',
          command: '',
          enabled: false,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })
  })
})

describe('getProjectBuildConfig', () => {
  it('returns the default config when build_config is absent', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p1', name: 'demo' }, null, 2), 'utf8')

    const result = await getProjectBuildConfig(metaPath)

    expect(result).toEqual({ ok: true, value: { enabled: false, steps: [] } })
  })

  it('repairs corrupted project.json via project meta helper and then reads build_config', async () => {
    await fs.writeFile(
      metaPath,
      '{\n  "id": "p2",\n  "build_config": {\n    "enabled": true,\n    "steps": [\n      {\n        "id": "compile",\n        "name": "Compile",\n        "envType": "visual-studio",\n        "cwd": "build",\n        "command": "cmake --build .",\n        "enabled": true\n      }\n    ]\n  }\n} trailing garbage',
      'utf8'
    )

    const result = await getProjectBuildConfig(metaPath)

    expect(result).toEqual({
      ok: true,
      repaired: true,
      value: {
        enabled: true,
        steps: [
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
          },
        ],
      },
    })
  })

  it('preserves invalid record-like steps on read instead of dropping them', async () => {
    await fs.writeFile(
      metaPath,
      JSON.stringify(
        {
          id: 'p-bad',
          build_config: {
            enabled: true,
            steps: [
              {
                id: '',
                name: ' Broken Step ',
                envType: 'powershell',
                cwd: ' build ',
                command: ' ',
              },
            ],
          },
        },
        null,
        2
      ),
      'utf8'
    )

    const result = await getProjectBuildConfig(metaPath)

    expect(result.ok).toBe(true)
    if (!result.ok) throw new Error('expected config')
    expect(result.value.enabled).toBe(true)
    expect(result.value.steps).toHaveLength(1)
    expect(result.value.steps[0]).toMatchObject({
      name: 'Broken Step',
      envType: 'msys',
      cwd: 'build',
      command: '',
      enabled: false,
    })
    expect(result.value.steps[0].id).not.toBe('')
  })
})

describe('setProjectBuildConfig', () => {
  it('writes build_config while preserving unrelated project meta fields', async () => {
    await fs.writeFile(
      metaPath,
      JSON.stringify(
        {
          id: 'p3',
          name: 'demo',
          target_repo: 'C:/repo',
          ai_settings: { ai_cli: 'claude' },
        },
        null,
        2
      ),
      'utf8'
    )

    const next: ProjectBuildConfig = {
      enabled: true,
      steps: [
        {
          id: 'configure',
          name: 'Configure',
          envType: 'msys',
          cwd: '.',
          command: 'cmake -S . -B build',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    }

    const result = await setProjectBuildConfig(metaPath, next)

    expect(result).toEqual({ ok: true })
    await expect(fs.readFile(metaPath, 'utf8')).resolves.toBe(
      '{\n' +
        '  "id": "p3",\n' +
        '  "name": "demo",\n' +
        '  "target_repo": "C:/repo",\n' +
        '  "ai_settings": {\n' +
        '    "ai_cli": "claude"\n' +
        '  },\n' +
        '  "build_config": {\n' +
        '    "enabled": true,\n' +
        '    "steps": [\n' +
        '      {\n' +
        '        "id": "configure",\n' +
        '        "name": "Configure",\n' +
        '        "envType": "msys",\n' +
        '        "cwd": ".",\n' +
        '        "command": "cmake -S . -B build",\n' +
        '        "enabled": true,\n' +
        '        "visualStudioInstanceId": "",\n' +
        '        "outputEncoding": "auto"\n' +
        '      }\n' +
        '    ]\n' +
        '  }\n' +
        '}'
    )
  })

  it('returns a structured error instead of silently dropping invalid steps', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p4', name: 'demo' }, null, 2), 'utf8')

    const result = await setProjectBuildConfig(metaPath, {
      enabled: true,
      steps: [
        {
          id: 'bad',
          name: 'Bad',
          envType: 'msys',
          cwd: '.',
          command: '',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })

    expect(result).toEqual({
      ok: false,
      error: 'invalid build config',
      details: [
        {
          path: 'build_config.steps[0].command',
          message: 'command must be a non-empty string',
        },
      ],
    })
  })

  it('returns a structured error for invalid envType on save', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p5', name: 'demo' }, null, 2), 'utf8')

    const result = await setProjectBuildConfig(metaPath, {
      enabled: true,
      steps: [
        {
          id: 'bad',
          name: 'Bad',
          envType: 'powershell' as never,
          cwd: '.',
          command: 'echo hi',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })

    expect(result).toEqual({
      ok: false,
      error: 'invalid build config',
      details: [
        {
          path: 'build_config.steps[0].envType',
          message: 'envType must be one of: system, msys, visual-studio',
        },
      ],
    })
  })

  it('rejects visual-studio steps with an empty visualStudioInstanceId on save', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p5a', name: 'demo' }, null, 2), 'utf8')

    const result = await setProjectBuildConfig(
      metaPath,
      {
        enabled: true,
        steps: [
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
            visualStudioInstanceId: '   ',
            outputEncoding: 'auto',
          },
        ],
      } as ProjectBuildConfig
    )

    expect(result).toEqual({
      ok: false,
      error: 'invalid build config',
      details: [
        {
          path: 'build_config.steps[0].visualStudioInstanceId',
          message: 'visualStudioInstanceId must be selected for visual-studio steps',
        },
      ],
    })
  })

  it('rejects invalid outputEncoding values on save', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p5b', name: 'demo' }, null, 2), 'utf8')

    const result = await setProjectBuildConfig(
      metaPath,
      {
        enabled: true,
        steps: [
          {
            id: 'compile',
            name: 'Compile',
            envType: 'msys',
            cwd: '.',
            command: 'cmake -S . -B build',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'latin1',
          },
        ],
      } as unknown as ProjectBuildConfig
    )

    expect(result).toEqual({
      ok: false,
      error: 'invalid build config',
      details: [
        {
          path: 'build_config.steps[0].outputEncoding',
          message: 'outputEncoding must be one of: auto, utf8, gbk',
        },
      ],
    })
  })

  it('normalizes before validate so an empty id is backfilled instead of rejected', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p6', name: 'demo' }, null, 2), 'utf8')

    const result = await setProjectBuildConfig(metaPath, {
      enabled: true,
      steps: [
        {
          id: '',
          name: 'Configure',
          envType: 'msys',
          cwd: '.',
          command: 'cmake -S . -B build',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })

    expect(result).toEqual({ ok: true })
    const written = JSON.parse(await fs.readFile(metaPath, 'utf8')) as {
      build_config: ProjectBuildConfig
    }
    expect(written.build_config.steps[0].id).toBeTruthy()
  })

  it('rejects absolute cwd on save', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p7', name: 'demo' }, null, 2), 'utf8')

    const result = await setProjectBuildConfig(metaPath, {
      enabled: true,
      steps: [
        {
          id: 'abs',
          name: 'Configure',
          envType: 'msys',
          cwd: 'C:\\outside',
          command: 'cmake -S . -B build',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })

    expect(result).toEqual({
      ok: false,
      error: 'invalid build config',
      details: [
        {
          path: 'build_config.steps[0].cwd',
          message: 'cwd must be a relative path within target_repo',
        },
      ],
    })
  })

  it('rejects parent traversal cwd on save', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p8', name: 'demo' }, null, 2), 'utf8')

    const result = await setProjectBuildConfig(metaPath, {
      enabled: true,
      steps: [
        {
          id: 'traversal',
          name: 'Configure',
          envType: 'msys',
          cwd: '..\\..\\outside',
          command: 'cmake -S . -B build',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
      ],
    })

    expect(result).toEqual({
      ok: false,
      error: 'invalid build config',
      details: [
        {
          path: 'build_config.steps[0].cwd',
          message: 'cwd must not contain parent traversal segments',
        },
      ],
    })
  })
})
