import { existsSync, readFileSync, writeFileSync } from 'fs'
import { join } from 'path'
import {
  binaryName,
  capture,
  copyExecutable,
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

requireDir(packageRoot, 'OpenCode submodule')

withPreservedFile(lockPath, () => {
  run(bun.command, [...bun.prefixArgs, 'install', '--frozen-lockfile', '--no-save'], {
    cwd: opencodeRoot
  })
  run(bun.command, [...bun.prefixArgs, 'run', '--cwd', 'packages/opencode', 'build', '--single'], {
    cwd: opencodeRoot
  })
})
copyExecutable(builtBinary, outputBinary)

writeManifestEntry({
  tool: 'opencode',
  platformArch: platform,
  sourceCommit: gitCommit(opencodeRoot),
  version: tryVersion(outputBinary),
  binaryPath: outputBinary
})

console.log(`OpenCode 已构建：${outputBinary}`)
