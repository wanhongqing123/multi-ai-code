import {
  chmodSync,
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync
} from 'fs'
import { dirname, join, relative, resolve } from 'path'
import { fileURLToPath } from 'url'
import { spawnSync } from 'child_process'

const scriptDir = dirname(fileURLToPath(import.meta.url))
export const repoRoot = resolve(scriptDir, '..')

export function platformArch() {
  return `${process.platform}-${process.arch}`
}

export function ensureDir(path) {
  mkdirSync(path, { recursive: true })
}

export function requireDir(path, label) {
  if (!existsSync(path)) {
    throw new Error(`${label} 不存在，请先初始化 submodule：git submodule update --init --recursive`)
  }
}

export function requireCommand(command, args = ['--version']) {
  const result = spawnSync(command, args, { encoding: 'utf8' })
  if (result.error || result.status !== 0) {
    throw new Error(`缺少命令 ${command}，请先安装后重试`)
  }
  return `${result.stdout ?? ''}${result.stderr ?? ''}`.trim()
}

export function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? repoRoot,
    env: { ...process.env, ...(options.env ?? {}) },
    stdio: 'inherit',
    shell: false
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(' ')} 执行失败，退出码 ${result.status}`)
  }
}

export function stripArgsForPlatform(platform = process.platform) {
  if (platform === 'darwin') return ['-S', '-x']
  if (platform === 'linux') return ['--strip-unneeded']
  return null
}

export function stripReleaseExecutable(
  binaryPath,
  { platform = process.platform, runCommand = run } = {}
) {
  const stripArgs = stripArgsForPlatform(platform)
  if (!stripArgs) return false
  runCommand('strip', [...stripArgs, binaryPath])
  return true
}

export function capture(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? repoRoot,
    env: { ...process.env, ...(options.env ?? {}) },
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    shell: false
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(' ')} 执行失败：${result.stderr || result.stdout}`)
  }
  return result.stdout.trim()
}

export function gitCommit(cwd) {
  return capture('git', ['rev-parse', 'HEAD'], { cwd })
}

export function binaryName(tool) {
  return process.platform === 'win32' ? `${tool}.exe` : tool
}

/**
 * 解析 bun/bunx 的真实可执行文件路径。
 * Windows 上 npm 全局安装的 bun 在 PATH 里只有 .cmd shim（%APPDATA%\npm\bunx.cmd），
 * 而本仓脚本统一 spawnSync(shell:false)，无法执行 .cmd —— 会直接 ENOENT。
 * 因此按常见安装位置探测真实 .exe：官方安装器（BUN_INSTALL / ~/.bun）与 npm 全局包目录；
 * 都不存在时回退裸命令名，交给 PATH（如用户把 exe 目录加进了 PATH 或非 Windows 平台）。
 */
export function resolveBunExecutable(
  name = 'bun',
  { platform = process.platform, env = process.env, exists = existsSync } = {}
) {
  if (platform !== 'win32') return name
  const candidates = [
    env.BUN_INSTALL ? join(env.BUN_INSTALL, 'bin', `${name}.exe`) : null,
    env.USERPROFILE ? join(env.USERPROFILE, '.bun', 'bin', `${name}.exe`) : null,
    env.APPDATA ? join(env.APPDATA, 'npm', 'node_modules', 'bun', 'bin', `${name}.exe`) : null
  ].filter(Boolean)
  for (const candidate of candidates) {
    if (exists(candidate)) return candidate
  }
  return name
}

export function copyExecutable(source, destination) {
  if (!existsSync(source)) {
    throw new Error(`构建产物不存在：${source}`)
  }
  ensureDir(dirname(destination))
  const tempDestination = `${destination}.${process.pid}.${Date.now()}.tmp`
  try {
    copyFileSync(source, tempDestination)
    if (process.platform !== 'win32') chmodSync(tempDestination, 0o755)
    renameSync(tempDestination, destination)
  } catch (error) {
    rmSync(tempDestination, { force: true })
    throw error
  }
}

export function readManifest(manifestPath) {
  if (!existsSync(manifestPath)) {
    return { generatedAt: null, entries: {} }
  }
  // Windows 工具（如 PowerShell）可能给 JSON 写入 UTF-8 BOM，JSON.parse 无法解析，先剥离。
  const raw = readFileSync(manifestPath, 'utf8')
  return JSON.parse(raw.charCodeAt(0) === 0xfeff ? raw.slice(1) : raw)
}

export function writeManifestEntry(entry) {
  const manifestPath = join(repoRoot, 'bin', 'aicli', 'manifest.json')
  ensureDir(dirname(manifestPath))
  const manifest = readManifest(manifestPath)
  manifest.generatedAt = new Date().toISOString()
  manifest.entries = manifest.entries ?? {}
  manifest.entries[entry.tool] = {
    ...entry,
    binaryPath: relative(repoRoot, entry.binaryPath).replaceAll('\\', '/')
  }
  writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, 'utf8')
}

export function tryVersion(binaryPath) {
  const result = spawnSync(binaryPath, ['--version'], {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    shell: false
  })
  if (result.error || result.status !== 0) return null
  return `${result.stdout ?? ''}${result.stderr ?? ''}`.trim().split(/\r?\n/)[0] || null
}
