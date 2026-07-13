import { existsSync, readdirSync, rmSync, statSync } from 'fs'
import { join } from 'path'

export function asrPlatformKey(platform, arch) {
  if (platform === 'darwin') return `darwin-${arch}`
  if (platform === 'win32') return `win32-${arch}`
  return `${platform}-${arch}`
}

export function parseAsrTargets(argv, fallbackTargets = ['all']) {
  const targets = []
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i]
    if (arg === '--target') {
      const target = argv[i + 1]
      if (!target) throw new Error('--target requires a value')
      targets.push(target)
      i += 1
      continue
    }
    if (arg.startsWith('--target=')) {
      targets.push(arg.slice('--target='.length))
      continue
    }
  }
  return targets.length > 0 ? targets : fallbackTargets
}

export function pruneAsrRuntimeForPlatform(asrRoot, { platform, arch }) {
  if (!existsSync(asrRoot)) return { asrRoot, removed: [], kept: [], missing: true }

  const keepDirs = new Set(['models', asrPlatformKey(platform, arch)])
  const removed = []
  const kept = []

  for (const name of readdirSync(asrRoot)) {
    const path = join(asrRoot, name)
    if (!statSync(path).isDirectory()) continue
    if (keepDirs.has(name)) {
      kept.push(name)
      continue
    }
    rmSync(path, { recursive: true, force: true })
    removed.push(name)
  }

  return { asrRoot, removed: removed.sort(), kept: kept.sort(), missing: false }
}

function findMacAsrRoot(appOutDir) {
  if (!existsSync(appOutDir)) return null
  for (const name of readdirSync(appOutDir)) {
    if (!name.endsWith('.app')) continue
    const candidate = join(appOutDir, name, 'Contents', 'Resources', 'asr')
    if (existsSync(candidate)) return candidate
  }
  return null
}

export function findPackagedAsrRoot(appOutDir, platform) {
  if (platform === 'darwin') return findMacAsrRoot(appOutDir)
  const candidate = join(appOutDir, 'resources', 'asr')
  return existsSync(candidate) ? candidate : null
}

export function prunePackagedAsrResources(appOutDir, { platform, arch }) {
  const asrRoot = findPackagedAsrRoot(appOutDir, platform)
  if (!asrRoot) {
    return { asrRoot: null, removed: [], kept: [], missing: true }
  }
  return pruneAsrRuntimeForPlatform(asrRoot, { platform, arch })
}
