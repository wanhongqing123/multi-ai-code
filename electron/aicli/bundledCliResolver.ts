import { existsSync, statSync } from 'fs'
import { basename, dirname, extname, isAbsolute, join, resolve } from 'path'
import { fileURLToPath } from 'url'

const __dirname = dirname(fileURLToPath(import.meta.url))

export type BundledCli = 'codex' | 'opencode'
export type AicliLaunchSource = 'bundled' | 'custom' | 'path'

export interface AicliLaunchDescription {
  tool: BundledCli
  label: 'Codex' | 'OpenCode'
  source: AicliLaunchSource
  commandPath: string
  notice: string
}

export interface BundledCliResolverOptions {
  platform?: NodeJS.Platform
  arch?: NodeJS.Architecture
  roots?: string[]
  existsFile?: (path: string) => boolean
}

function normalizeCliCommand(command: string): string {
  let normalized = command.trim()
  while (normalized.length >= 2) {
    const first = normalized[0]
    const last = normalized[normalized.length - 1]
    if ((first === '"' && last === '"') || (first === "'" && last === "'")) {
      normalized = normalized.slice(1, -1).trim()
      continue
    }
    break
  }
  return normalized
}

export function bundledCliFromCommand(command: string): BundledCli | null {
  const normalized = normalizeCliCommand(command)
  if (isAbsolute(normalized) || normalized.includes('/') || normalized.includes('\\')) {
    return null
  }
  const base = basename(normalized).toLowerCase()
  if (/^codex(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'codex'
  if (/^opencode(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'opencode'
  return null
}

function aicliFromAnyCommand(command: string): BundledCli | null {
  const normalized = normalizeCliCommand(command)
  const base = basename(normalized).toLowerCase()
  if (/^codex(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'codex'
  if (/^opencode(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'opencode'
  return null
}

function bundledCliLabel(tool: BundledCli): 'Codex' | 'OpenCode' {
  return tool === 'codex' ? 'Codex' : 'OpenCode'
}

function commandLooksLikeCustomPath(command: string): boolean {
  const normalized = normalizeCliCommand(command)
  return isAbsolute(normalized) || normalized.includes('/') || normalized.includes('\\')
}

export function describeAicliLaunchCommand(
  configuredCommand: string,
  resolvedCommand: string,
  bundledCommand: string | null
): AicliLaunchDescription | null {
  const tool = aicliFromAnyCommand(configuredCommand) ?? aicliFromAnyCommand(resolvedCommand)
  if (!tool) return null

  const label = bundledCliLabel(tool)
  const source: AicliLaunchSource = bundledCommand
    ? 'bundled'
    : commandLooksLikeCustomPath(configuredCommand)
      ? 'custom'
      : 'path'
  const sourceLabel =
    source === 'bundled' ? '内置版本' : source === 'custom' ? '自定义路径' : '系统 PATH'

  return {
    tool,
    label,
    source,
    commandPath: resolvedCommand,
    notice: `当前启动 ${label}：${sourceLabel} ${resolvedCommand}`
  }
}

export function bundledPlatformArch(
  platform: NodeJS.Platform = process.platform,
  arch: NodeJS.Architecture = process.arch
): string {
  return `${platform}-${arch}`
}

function bundledBinaryName(tool: BundledCli, platform: NodeJS.Platform): string {
  return platform === 'win32' ? `${tool}.exe` : tool
}

function defaultRoots(): string[] {
  const roots = [
    join(process.cwd(), 'bin', 'aicli'),
    resolve(__dirname, '..', '..', 'bin', 'aicli'),
    resolve(__dirname, '..', '..', '..', 'bin', 'aicli')
  ]
  if (process.resourcesPath) {
    // Packaged app: native CLI binaries are asar-unpacked, so the real files
    // live under `app.asar.unpacked/bin/aicli`. A path *inside* app.asar is not
    // executable by native launchers (node-pty bypasses Electron's asar path
    // translation), which is why launching produced
    // "File not found: ...\\app.asar\\bin\\aicli\\...\\opencode.exe".
    // Prefer the on-disk unpacked location. In dev these two candidates do not
    // exist (resourcesPath points at the prebuilt Electron), so resolution
    // still falls through to cwd/__dirname → the repo's own bin/aicli.
    roots.unshift(join(process.resourcesPath, 'bin', 'aicli'))
    roots.unshift(join(process.resourcesPath, 'app.asar.unpacked', 'bin', 'aicli'))
  }
  return Array.from(new Set(roots))
}

function defaultExistsFile(path: string): boolean {
  try {
    // A path inside the packed asar archive (but not the unpacked sibling) is
    // not spawnable by native launchers, so never treat it as a valid bundled
    // binary even though Electron's asar-aware fs would report it as existing.
    if (/[\\/]app\.asar[\\/]/.test(path) && !/[\\/]app\.asar\.unpacked[\\/]/.test(path)) {
      return false
    }
    return existsSync(path) && statSync(path).isFile()
  } catch {
    return false
  }
}

function resolveBundledCliForTool(
  tool: BundledCli,
  options: BundledCliResolverOptions = {}
): string | null {
  const platform = options.platform ?? process.platform
  const arch = options.arch ?? process.arch
  const existsFile = options.existsFile ?? defaultExistsFile
  const binary = bundledBinaryName(tool, platform)
  const platformArch = bundledPlatformArch(platform, arch)

  for (const root of options.roots ?? defaultRoots()) {
    const candidate = join(root, tool, platformArch, binary)
    if (existsFile(candidate)) return candidate
  }
  return null
}

export function resolveBundledCliCommand(
  command: string,
  options: BundledCliResolverOptions = {}
): string | null {
  const tool = bundledCliFromCommand(command)
  if (!tool) return null
  return resolveBundledCliForTool(tool, options)
}

export interface AicliCommandResolution {
  // codex / opencode 时非空；claude 及其它命令为 null（不受内置约束）。
  tool: BundledCli | null
  label: 'Codex' | 'OpenCode' | null
  // 内置二进制的绝对路径；codex/opencode 解析到内置时非空。
  bundledCommand: string | null
  // codex/opencode 但没找到内置二进制：调用方必须报错，绝不回退宿主机版本。
  bundledMissing: boolean
}

// codex / opencode 已随应用深度定制，策略：只允许运行随应用打包的「内置」版本。
// 无论配置成裸命令 "codex"/"opencode" 还是宿主机上的 PATH / 自定义路径，一律解析到
// 内置二进制；解析不到就置 bundledMissing（调用方据此报错，绝不回退到宿主机自行安装的版本）。
// claude 及其它命令返回 tool=null，不受影响，仍按原来的 PATH / 自定义路径解析。
export function resolveAicliCommand(
  command: string,
  options: BundledCliResolverOptions = {}
): AicliCommandResolution {
  const tool = aicliFromAnyCommand(command)
  if (!tool) {
    return { tool: null, label: null, bundledCommand: null, bundledMissing: false }
  }
  const bundledCommand = resolveBundledCliForTool(tool, options)
  return {
    tool,
    label: bundledCliLabel(tool),
    bundledCommand,
    bundledMissing: bundledCommand === null
  }
}

// codex/opencode 必须用内置版本时，找不到内置二进制的统一报错文案。
export function bundledCliMissingMessage(resolution: AicliCommandResolution): string {
  const label = resolution.label ?? 'AICLI'
  const tool = resolution.tool ?? ''
  return (
    `未找到内置的 ${label} 可执行文件。${label}（codex / opencode）已随应用深度定制，` +
    `仅支持随应用打包的内置版本，无法使用宿主机上自行安装的 ${tool}。` +
    `请重新安装应用，或在源码仓库执行 \`npm run build:aicli\` 生成 bin/aicli 下的二进制。`
  )
}
