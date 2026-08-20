import { copyFileSync, existsSync, readFileSync, writeFileSync } from 'fs'
import { createHash } from 'crypto'
import { dirname, join } from 'path'
import {
  binaryName,
  capture,
  copyExecutable,
  ensureDir,
  gitCommit,
  platformArch,
  repoRoot,
  requireDir,
  resolveBunExecutable,
  run,
  tryVersion,
  writeManifestEntry
} from './aicli-build-utils.mjs'

const REQUIRED_BUN = '1.3.14'

function parseVersion(raw) {
  const match = raw.trim().match(/^(\d+)\.(\d+)\.(\d+)/)
  if (!match) return null
  return match.slice(1).map((part) => Number(part))
}

function satisfiesBun(raw) {
  const current = parseVersion(raw)
  const required = parseVersion(REQUIRED_BUN)
  if (!current || !required) return false
  if (current[0] !== required[0]) return false
  if (current[1] !== required[1]) return current[1] > required[1]
  return current[2] >= required[2]
}

function bunCommand() {
  // 先解析真实 exe（Windows 上 npm shim 是 .cmd，spawnSync(shell:false) 执行不了）。
  const bunBinary = resolveBunExecutable('bun')
  try {
    const version = capture(bunBinary, ['--version'])
    if (satisfiesBun(version)) return { command: bunBinary, prefixArgs: [] }
    // bun 存在但版本过旧：用它的 `bun x` 跑固定版本，等价 bunx 且不依赖 bunx.exe。
    return { command: bunBinary, prefixArgs: ['x', `bun@${REQUIRED_BUN}`] }
  } catch {
    // 完全找不到 bun，再试 bunx
  }
  const bunxBinary = resolveBunExecutable('bunx')
  if (bunxBinary !== 'bunx' || process.platform !== 'win32') {
    return { command: bunxBinary, prefixArgs: [`bun@${REQUIRED_BUN}`] }
  }
  throw new Error(
    '未找到可用的 bun/bunx 可执行文件。请安装 bun（npm install -g bun 或 https://bun.sh 官方安装器）后重试。'
  )
}

function opencodeDistName(platform) {
  const [os, arch] = platform.split('-')
  return `opencode-${os === 'win32' ? 'windows' : os}-${arch}`
}

function firstExisting(paths) {
  return paths.find((item) => existsSync(item)) ?? paths[0]
}

function withPreservedFile(path, callback) {
  const original = existsSync(path) ? readFileSync(path) : null
  try {
    callback()
  } finally {
    if (original) {
      writeFileSync(path, original)
    }
  }
}

const opencodeRoot = join(repoRoot, 'third_party', 'aicli', 'opencode')
const packageRoot = join(opencodeRoot, 'packages', 'opencode')
const platform = platformArch()
const outputBinary = join(repoRoot, 'bin', 'aicli', 'opencode', platform, binaryName('opencode'))
const distBinDir = join(packageRoot, 'dist', opencodeDistName(platform), 'bin')
const builtBinary = firstExisting([
  join(distBinDir, binaryName('opencode')),
  join(distBinDir, 'opencode')
])
const bun = bunCommand()
const lockPath = join(opencodeRoot, 'bun.lock')
const managedModelsPath = join(repoRoot, 'resources', 'opencode', 'managed-models.json')
const managedRoutingPath = join(repoRoot, 'resources', 'opencode', 'managed-routing.json')
const managedProfilePath = join(repoRoot, 'resources', 'opencode', 'managed-profile.json')

function readJson(path, label) {
  if (!existsSync(path)) throw new Error(`${label} 不存在：${path}`)
  try {
    return JSON.parse(readFileSync(path, 'utf8'))
  } catch (error) {
    throw new Error(`${label} 不是有效 JSON：${error instanceof Error ? error.message : String(error)}`)
  }
}

function validateManagedAssets() {
  const catalog = readJson(managedModelsPath, 'OpenCode 受控模型目录')
  const routing = readJson(managedRoutingPath, 'OpenCode 受控路由目录')
  const profile = readJson(managedProfilePath, 'OpenCode 托管 Profile')
  if (!catalog || typeof catalog !== 'object' || Array.isArray(catalog)) {
    throw new Error('OpenCode 受控模型目录根节点必须是 Provider 对象')
  }
  if (!routing || routing.version !== 1 || !routing.models || typeof routing.models !== 'object') {
    throw new Error('OpenCode 受控路由目录必须包含 version=1 和 models')
  }
  for (const [providerId, provider] of Object.entries(catalog)) {
    if (!provider || typeof provider !== 'object' || provider.id !== providerId) {
      throw new Error(`OpenCode 受控模型目录中的 Provider ID 不一致：${providerId}`)
    }
    if (!provider.models || typeof provider.models !== 'object') {
      throw new Error(`OpenCode Provider 缺少 models：${providerId}`)
    }
    for (const [modelId, model] of Object.entries(provider.models)) {
      if (!model || typeof model !== 'object' || model.id !== modelId) {
        throw new Error(`OpenCode 模型 ID 不一致：${providerId}/${modelId}`)
      }
    }
  }
  for (const modelRef of Object.keys(routing.models)) {
    const slash = modelRef.indexOf('/')
    const providerId = slash > 0 ? modelRef.slice(0, slash) : ''
    const modelId = slash > 0 ? modelRef.slice(slash + 1) : ''
    if (!providerId || !modelId || !catalog[providerId]?.models?.[modelId]) {
      throw new Error(`OpenCode 路由引用了不存在的模型：${modelRef}`)
    }
  }
  if (!profile || profile.version !== 1 ||
      !Array.isArray(profile.enabledProviders) || !profile.enabledProviders.length) {
    throw new Error('OpenCode 托管 Profile Schema 无效')
  }
  for (const modelRef of [profile.defaultModel, profile.smallModel]) {
    const slash = typeof modelRef === 'string' ? modelRef.indexOf('/') : -1
    const providerId = slash > 0 ? modelRef.slice(0, slash) : ''
    const modelId = slash > 0 ? modelRef.slice(slash + 1) : ''
    if (!providerId || !modelId || !catalog[providerId]?.models?.[modelId]) {
      throw new Error(`OpenCode 托管 Profile 引用了不存在的模型：${modelRef}`)
    }
  }
  for (const providerId of profile.enabledProviders) {
    const provider = catalog[providerId]
    if (!provider) {
      throw new Error(`OpenCode 托管 Profile 缺少 Provider：${providerId}`)
    }
    if (!Array.isArray(provider.env) || !provider.env.length) {
      throw new Error(`OpenCode 托管 Provider 未声明 API Key 环境变量：${providerId}`)
    }
  }
}

function sha256(path) {
  return createHash('sha256').update(readFileSync(path)).digest('hex')
}

function copyManagedAssets() {
  const outputDir = dirname(outputBinary)
  ensureDir(outputDir)
  const modelsOutput = join(outputDir, 'managed-models.json')
  const routingOutput = join(outputDir, 'managed-routing.json')
  const profileOutput = join(outputDir, 'managed-profile.json')
  copyFileSync(managedModelsPath, modelsOutput)
  copyFileSync(managedRoutingPath, routingOutput)
  copyFileSync(managedProfilePath, profileOutput)
  writeFileSync(
    join(outputDir, 'managed-assets.json'),
    `${JSON.stringify(
      {
        version: 1,
        files: {
          'managed-models.json': sha256(modelsOutput),
          'managed-routing.json': sha256(routingOutput),
          'managed-profile.json': sha256(profileOutput)
        }
      },
      null,
      2
    )}\n`,
    'utf8'
  )
}

requireDir(packageRoot, 'OpenCode submodule')
validateManagedAssets()

withPreservedFile(lockPath, () => {
  run(bun.command, [...bun.prefixArgs, 'install', '--frozen-lockfile', '--no-save'], {
    cwd: opencodeRoot
  })
  run(bun.command, [...bun.prefixArgs, 'run', '--cwd', 'packages/opencode', 'build', '--single'], {
    cwd: opencodeRoot,
    env: { MODELS_DEV_API_JSON: managedModelsPath }
  })
})
copyExecutable(builtBinary, outputBinary)
copyManagedAssets()

writeManifestEntry({
  tool: 'opencode',
  platformArch: platform,
  sourceCommit: gitCommit(opencodeRoot),
  version: tryVersion(outputBinary),
  binaryPath: outputBinary
})

console.log(`OpenCode 已构建：${outputBinary}`)
