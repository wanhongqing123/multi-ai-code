import { createHash } from 'crypto'
import { existsSync, readFileSync, statSync } from 'fs'
import { dirname, join } from 'path'
import {
  resolveAicliCommand,
  type BundledCliResolverOptions
} from './bundledCliResolver.js'
import {
  isOpenCodeCommand,
  withOpenCodeManagedProfileEnv,
  type OpenCodeManagedProfile
} from './opencodeConfig.js'

export const OPENCODE_RUNTIME_ROOT_ENV = 'OPENCODE_RUNTIME_ROOT'
export const OPENCODE_MODELS_PATH_ENV = 'OPENCODE_MODELS_PATH'
export const OPENCODE_MANAGED_ROUTING_PATH_ENV = 'OPENCODE_MANAGED_ROUTING_PATH'

interface ManagedAssetManifest {
  version: number
  files: Record<string, string>
}

export interface OpenCodeManagedAssets {
  binaryPath: string
  modelsPath: string
  routingPath: string
  profilePath: string
  manifestPath: string
}

function requireRegularFile(path: string, label: string): void {
  try {
    if (existsSync(path) && statSync(path).isFile()) return
  } catch {
    // Unified error below keeps the startup failure actionable.
  }
  throw new Error(`OpenCode 安装资源损坏：缺少 ${label}（${path}）`)
}

function sha256(path: string): string {
  return createHash('sha256').update(readFileSync(path)).digest('hex')
}

function readManifest(path: string): ManagedAssetManifest {
  let value: unknown
  try {
    value = JSON.parse(readFileSync(path, 'utf8'))
  } catch (error) {
    throw new Error(
      `OpenCode 安装资源损坏：无法读取受控资源清单（${error instanceof Error ? error.message : String(error)}）`
    )
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('OpenCode 安装资源损坏：受控资源清单格式无效')
  }
  const manifest = value as Partial<ManagedAssetManifest>
  if (manifest.version !== 1 || !manifest.files || typeof manifest.files !== 'object') {
    throw new Error('OpenCode 安装资源损坏：受控资源清单版本或文件列表无效')
  }
  return manifest as ManagedAssetManifest
}

function validateJson(path: string, label: string): Record<string, unknown> {
  try {
    const value = JSON.parse(readFileSync(path, 'utf8'))
    if (value && typeof value === 'object' && !Array.isArray(value)) {
      return value as Record<string, unknown>
    }
  } catch {
    // Unified error below.
  }
  throw new Error(`OpenCode 安装资源损坏：${label} 不是有效的 JSON 对象`)
}

function validateManagedProfile(
  value: Record<string, unknown>,
  catalog: Record<string, unknown>
): OpenCodeManagedProfile {
  if (value.version !== 1) throw new Error('OpenCode 安装资源损坏：managed-profile.json 版本无效')
  const defaultModel = typeof value.defaultModel === 'string' ? value.defaultModel : ''
  const smallModel = typeof value.smallModel === 'string' ? value.smallModel : ''
  const enabledProviders = Array.isArray(value.enabledProviders)
    ? value.enabledProviders.filter((item): item is string => typeof item === 'string' && item.length > 0)
    : []
  const providers = value.providers
  if (!defaultModel || !smallModel || !enabledProviders.length || !providers ||
      typeof providers !== 'object' || Array.isArray(providers)) {
    throw new Error('OpenCode 安装资源损坏：managed-profile.json Schema 无效')
  }

  const requireModel = (modelRef: string): void => {
    const separator = modelRef.indexOf('/')
    const providerId = separator > 0 ? modelRef.slice(0, separator) : ''
    const modelId = separator > 0 ? modelRef.slice(separator + 1) : ''
    const provider = catalog[providerId]
    const models = provider && typeof provider === 'object' && !Array.isArray(provider)
      ? (provider as Record<string, unknown>).models
      : undefined
    if (!models || typeof models !== 'object' || Array.isArray(models) || !(modelId in models)) {
      throw new Error(`OpenCode 安装资源损坏：托管 Profile 引用了不存在的模型 ${modelRef}`)
    }
  }
  requireModel(defaultModel)
  requireModel(smallModel)

  const normalizedProviders: OpenCodeManagedProfile['providers'] = {}
  for (const providerId of enabledProviders) {
    const catalogProvider = catalog[providerId]
    const profileProvider = (providers as Record<string, unknown>)[providerId]
    if (!catalogProvider || typeof catalogProvider !== 'object' || Array.isArray(catalogProvider) ||
        !profileProvider || typeof profileProvider !== 'object' || Array.isArray(profileProvider)) {
      throw new Error(`OpenCode 安装资源损坏：托管 Provider 不存在 ${providerId}`)
    }
    const declaredEnv = new Set(
      Array.isArray((catalogProvider as Record<string, unknown>).env)
        ? ((catalogProvider as Record<string, unknown>).env as unknown[]).filter(
            (item): item is string => typeof item === 'string'
          )
        : []
    )
    const env = (profileProvider as Record<string, unknown>).env
    if (!env || typeof env !== 'object' || Array.isArray(env)) {
      throw new Error(`OpenCode 安装资源损坏：托管 Provider 缺少凭据 ${providerId}`)
    }
    const normalizedEnv: Record<string, string> = {}
    for (const [name, secret] of Object.entries(env)) {
      if (!declaredEnv.has(name) || typeof secret !== 'string' || !secret) {
        throw new Error(`OpenCode 安装资源损坏：托管 Provider 凭据无效 ${providerId}/${name}`)
      }
      normalizedEnv[name] = secret
    }
    if (!Object.keys(normalizedEnv).length) {
      throw new Error(`OpenCode 安装资源损坏：托管 Provider 没有可用凭据 ${providerId}`)
    }
    normalizedProviders[providerId] = { env: normalizedEnv }
  }
  return {
    version: 1,
    defaultModel,
    smallModel,
    enabledProviders,
    providers: normalizedProviders
  }
}

export function resolveOpenCodeManagedAssets(
  command: string,
  options: BundledCliResolverOptions = {}
): OpenCodeManagedAssets {
  const resolution = resolveAicliCommand(command, options)
  if (resolution.tool !== 'opencode' || !resolution.bundledCommand) {
    throw new Error('无法解析内置 OpenCode，可执行文件缺失')
  }
  const directory = dirname(resolution.bundledCommand)
  const assets: OpenCodeManagedAssets = {
    binaryPath: resolution.bundledCommand,
    modelsPath: join(directory, 'managed-models.json'),
    routingPath: join(directory, 'managed-routing.json'),
    profilePath: join(directory, 'managed-profile.json'),
    manifestPath: join(directory, 'managed-assets.json')
  }
  requireRegularFile(assets.modelsPath, 'managed-models.json')
  requireRegularFile(assets.routingPath, 'managed-routing.json')
  requireRegularFile(assets.profilePath, 'managed-profile.json')
  requireRegularFile(assets.manifestPath, 'managed-assets.json')

  const manifest = readManifest(assets.manifestPath)
  for (const [name, path] of [
    ['managed-models.json', assets.modelsPath],
    ['managed-routing.json', assets.routingPath],
    ['managed-profile.json', assets.profilePath]
  ] as const) {
    const expected = manifest.files[name]
    if (!/^[a-f0-9]{64}$/i.test(expected ?? '') || sha256(path) !== expected) {
      throw new Error(`OpenCode 安装资源损坏：${name} 完整性校验失败`)
    }
  }

  const catalog = validateJson(assets.modelsPath, 'managed-models.json')
  const routing = validateJson(assets.routingPath, 'managed-routing.json')
  const profile = validateJson(assets.profilePath, 'managed-profile.json')
  const routes = routing.models
  if (routing.version !== 1 || !routes || typeof routes !== 'object' || Array.isArray(routes)) {
    throw new Error('OpenCode 安装资源损坏：managed-routing.json Schema 无效')
  }
  for (const modelRef of Object.keys(routes)) {
    const separator = modelRef.indexOf('/')
    const providerId = separator > 0 ? modelRef.slice(0, separator) : ''
    const modelId = separator > 0 ? modelRef.slice(separator + 1) : ''
    const provider = catalog[providerId]
    const models =
      provider && typeof provider === 'object' && !Array.isArray(provider)
        ? (provider as Record<string, unknown>).models
        : undefined
    if (!models || typeof models !== 'object' || Array.isArray(models) || !(modelId in models)) {
      throw new Error(`OpenCode 安装资源损坏：路由引用了不存在的模型 ${modelRef}`)
    }
  }
  validateManagedProfile(profile, catalog)
  return assets
}

export function withOpenCodeManagedRuntimeEnv(
  command: string,
  env: Record<string, string> | undefined,
  runtimeRoot: string,
  options: BundledCliResolverOptions = {}
): Record<string, string> | undefined {
  if (!isOpenCodeCommand(command)) return env
  const assets = resolveOpenCodeManagedAssets(command, options)
  const profile = validateManagedProfile(
    validateJson(assets.profilePath, 'managed-profile.json'),
    validateJson(assets.modelsPath, 'managed-models.json')
  )
  return withOpenCodeManagedProfileEnv({
    ...(env ?? {}),
    [OPENCODE_RUNTIME_ROOT_ENV]: runtimeRoot,
    [OPENCODE_MODELS_PATH_ENV]: assets.modelsPath,
    [OPENCODE_MANAGED_ROUTING_PATH_ENV]: assets.routingPath
  }, profile)
}
