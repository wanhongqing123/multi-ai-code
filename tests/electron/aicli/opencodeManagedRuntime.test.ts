import { createHash } from 'crypto'
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { afterEach, describe, expect, it } from 'vitest'
import {
  resolveOpenCodeManagedAssets,
  withOpenCodeManagedRuntimeEnv
} from '../../../electron/aicli/opencodeManagedRuntime.js'

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
  writeFileSync(binaryPath, '')
  writeFileSync(
    modelsPath,
    JSON.stringify({
      provider: {
        id: 'provider',
        env: ['PROVIDER_API_KEY'],
        models: { model: { id: 'model' } }
      }
    })
  )
  writeFileSync(
    routingPath,
    JSON.stringify({ version: 1, models: { 'provider/model': { roles: ['default_text'] } } })
  )
  writeFileSync(
    profilePath,
    JSON.stringify({
      version: 1,
      defaultModel: 'provider/model',
      smallModel: 'provider/model',
      enabledProviders: ['provider'],
      providers: { provider: { env: { PROVIDER_API_KEY: 'managed-secret' } } }
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
  return { root, binaryPath, modelsPath, routingPath, profilePath, manifestPath }
}

describe('OpenCode managed runtime', () => {
  it('uses the Coding Plan endpoint for the managed Zhipu provider', () => {
    const catalog = JSON.parse(
      readFileSync(join(process.cwd(), 'resources', 'opencode', 'managed-models.json'), 'utf8')
    )
    expect(catalog.zhipu.api).toBe('https://open.bigmodel.cn/api/coding/paas/v4')
    expect(catalog.zhipu.models['glm-4.6v']?.attachment).toBe(true)
    expect(catalog.zhipu.models['glm-5v-turbo']?.attachment).toBe(true)

    const routing = JSON.parse(
      readFileSync(join(process.cwd(), 'resources', 'opencode', 'managed-routing.json'), 'utf8')
    )
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

  it('injects account runtime and managed resource paths without user setup', () => {
    const files = fixture()
    const env = withOpenCodeManagedRuntimeEnv('opencode', { FOO: 'bar' }, '/accounts/alice/aicli/opencode', {
      platform: 'darwin',
      arch: 'arm64',
      roots: [files.root]
    })
    expect(env).toMatchObject({
      FOO: 'bar',
      OPENCODE_RUNTIME_ROOT: '/accounts/alice/aicli/opencode',
      OPENCODE_MODELS_PATH: files.modelsPath,
      OPENCODE_MANAGED_ROUTING_PATH: files.routingPath,
      PROVIDER_API_KEY: 'managed-secret'
    })
    expect(JSON.parse(env?.OPENCODE_CONFIG_CONTENT ?? '{}')).toMatchObject({
      model: 'provider/model',
      small_model: 'provider/model',
      enabled_providers: ['provider']
    })
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
