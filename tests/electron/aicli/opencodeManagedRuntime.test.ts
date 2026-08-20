import { createHash } from 'crypto'
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { afterEach, describe, expect, it } from 'vitest'
import {
  resolveOpenCodeManagedAssets,
  withOpenCodeManagedRuntimeEnv
} from '../../../electron/aicli/opencodeManagedRuntime.js'
import { writeOpenCodeApiKey } from '../../../electron/aicli/opencodeCredentials.js'

const roots: string[] = []

afterEach(() => {
  for (const root of roots.splice(0)) rmSync(root, { recursive: true, force: true })
})

function sha256(path: string): string {
  return createHash('sha256').update(readFileSync(path)).digest('hex')
}

function fixture() {
  const root = mkdtempSync(join(tmpdir(), 'opencode-managed-'))
  roots.push(root)
  const directory = join(root, 'opencode', 'darwin-arm64')
  mkdirSync(directory, { recursive: true })
  const binaryPath = join(directory, 'opencode')
  const modelsPath = join(directory, 'managed-models.json')
  const routingPath = join(directory, 'managed-routing.json')
  const profilePath = join(directory, 'managed-profile.json')
  const manifestPath = join(directory, 'managed-assets.json')
  const runtimeRoot = join(root, 'runtime')
  writeFileSync(binaryPath, '')
  writeFileSync(
    modelsPath,
    JSON.stringify({
      zhipu: {
        id: 'zhipu',
        env: ['ZHIPU_API_KEY'],
        models: { glm: { id: 'glm' } }
      }
    })
  )
  writeFileSync(
    routingPath,
    JSON.stringify({ version: 1, models: { 'zhipu/glm': { roles: ['default_text'] } } })
  )
  writeFileSync(
    profilePath,
    JSON.stringify({
      version: 1,
      defaultModel: 'zhipu/glm',
      smallModel: 'zhipu/glm',
      enabledProviders: ['zhipu']
    })
  )
  writeFileSync(
    manifestPath,
    JSON.stringify({
      version: 1,
      files: {
        'managed-models.json': sha256(modelsPath),
        'managed-routing.json': sha256(routingPath),
        'managed-profile.json': sha256(profilePath)
      }
    })
  )
  return { root, runtimeRoot, binaryPath, modelsPath, routingPath, profilePath, manifestPath }
}

describe('OpenCode managed runtime', () => {
  it('exposes only managed GLM models through the Coding Plan endpoint', () => {
    const catalog = JSON.parse(
      readFileSync(join(process.cwd(), 'resources', 'opencode', 'managed-models.json'), 'utf8')
    )
    expect(Object.keys(catalog)).toEqual(['zhipu'])
    expect(catalog.zhipu.api).toBe('https://open.bigmodel.cn/api/coding/paas/v4')
    expect(Object.keys(catalog.zhipu.models)).toEqual([
      'glm-5.3',
      'glm-5.2',
      'glm-5v-turbo',
      'glm-4.6v'
    ])
    expect(catalog.zhipu.models['glm-5.3']).toMatchObject({
      attachment: false,
      reasoning: true,
      reasoning_options: [{ type: 'effort', values: ['low', 'high', 'max'] }],
      limit: { context: 1000000, output: 131072 }
    })
    expect(catalog.zhipu.models['glm-4.6v']?.attachment).toBe(true)
    expect(catalog.zhipu.models['glm-5v-turbo']?.attachment).toBe(true)

    const profile = JSON.parse(
      readFileSync(join(process.cwd(), 'resources', 'opencode', 'managed-profile.json'), 'utf8')
    )
    expect(profile).toMatchObject({
      defaultModel: 'zhipu/glm-5.3',
      smallModel: 'zhipu/glm-5.3',
      enabledProviders: ['zhipu']
    })
    expect(profile).not.toHaveProperty('providers')

    const routing = JSON.parse(
      readFileSync(join(process.cwd(), 'resources', 'opencode', 'managed-routing.json'), 'utf8')
    )
    expect(Object.keys(routing.models).every((modelRef) => modelRef.startsWith('zhipu/glm-'))).toBe(true)
    expect(routing.models['zhipu/glm-5.3']).toMatchObject({
      roles: ['default_text', 'strong_text', 'small'],
      priority: 110
    })
    expect(routing.models['zhipu/glm-5.2']).toMatchObject({
      roles: ['default_text', 'strong_text', 'small'],
      priority: 100
    })
    expect(routing.models['zhipu/glm-5v-turbo']).toMatchObject({ roles: ['vision'], priority: 100 })
    expect(routing.models['zhipu/glm-4.6v']).toMatchObject({ roles: ['vision'], priority: 80 })
  })

  it('resolves and verifies resources beside the bundled binary', () => {
    const files = fixture()
    expect(
      resolveOpenCodeManagedAssets('opencode', {
        platform: 'darwin',
        arch: 'arm64',
        roots: [files.root]
      })
    ).toEqual({
      binaryPath: files.binaryPath,
      modelsPath: files.modelsPath,
      routingPath: files.routingPath,
      profilePath: files.profilePath,
      manifestPath: files.manifestPath
    })
  })

  it('injects the account-local API Key with managed runtime paths', () => {
    const files = fixture()
    writeOpenCodeApiKey(files.runtimeRoot, 'managed-secret')
    const env = withOpenCodeManagedRuntimeEnv('opencode', { FOO: 'bar' }, files.runtimeRoot, {
      platform: 'darwin',
      arch: 'arm64',
      roots: [files.root]
    })
    expect(env).toMatchObject({
      FOO: 'bar',
      OPENCODE_RUNTIME_ROOT: files.runtimeRoot,
      OPENCODE_MODELS_PATH: files.modelsPath,
      OPENCODE_MANAGED_ROUTING_PATH: files.routingPath,
      ZHIPU_API_KEY: 'managed-secret'
    })
    expect(JSON.parse(env?.OPENCODE_CONFIG_CONTENT ?? '{}')).toMatchObject({
      model: 'zhipu/glm',
      small_model: 'zhipu/glm',
      enabled_providers: ['zhipu']
    })
  })

  it('requires the account-local API Key before starting OpenCode', () => {
    const files = fixture()
    expect(() =>
      withOpenCodeManagedRuntimeEnv('opencode', {}, files.runtimeRoot, {
        platform: 'darwin',
        arch: 'arm64',
        roots: [files.root]
      })
    ).toThrow('请先在设置中填写')
  })

  it('rejects a modified catalog instead of falling back to host caches', () => {
    const files = fixture()
    writeFileSync(files.modelsPath, '{}')
    expect(() =>
      resolveOpenCodeManagedAssets('opencode', {
        platform: 'darwin',
        arch: 'arm64',
        roots: [files.root]
      })
    ).toThrow('完整性校验失败')
  })

  it('does not touch non-OpenCode commands', () => {
    expect(withOpenCodeManagedRuntimeEnv('codex', { FOO: 'bar' }, '/runtime')).toEqual({ FOO: 'bar' })
  })
})
