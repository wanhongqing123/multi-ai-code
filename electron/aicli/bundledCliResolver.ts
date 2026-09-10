import { existsSync, statSync } from 'fs'
import { basename, dirname, extname, isAbsolute, join, resolve } from 'path'
import { fileURLToPath } from 'url'

const __dirname = dirname(fileURLToPath(import.meta.url))

export type BundledCli = 'codex'
export type BundledCliLabel = 'Codex'
export type AicliLaunchSource = 'bundled' | 'custom' | 'path'

// 内置 AICLI 注册表。判定曾经散在四处——两个各写一遍的
// 正则、一个 label 三元表达式、一句写死内置清单的报错文案——每加一种
// 都要同步改四个地方，漏掉任何一个都不会有测试报错。改成一张表之后，这些派生逻辑
// 只从表里读，新增一种 AICLI 只动这一处。
const BUNDLED_CLIS: readonly { id: BundledCli; label: BundledCliLabel }[] = [
  { id: 'codex', label: 'Codex' },
]

// 只匹配裸命令名（可带 Windows 可执行后缀）。**两端锚定是必须的**：
// codex 那边有 codex-code-mode-host——少了 $
// 锚点它们都会被认成对应的内置 CLI，进而被强制改写成主二进制去执行。
// （这个缺陷对原来那套用例是隐形的：它只测了前缀不匹配，没有一条测后缀。）
function matchesBundledCli(base: string, tool: BundledCli): boolean {
  return new RegExp(`^${tool}(\\.(exe|cmd|bat|ps1))?$`).test(base)
}

function bundledCliFromBaseName(base: string): BundledCli | null {
  return BUNDLED_CLIS.find((entry) => matchesBundledCli(base, entry.id))?.id ?? null
}

export interface AicliLaunchDescription {
  tool: BundledCli
  label: BundledCliLabel
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
  return bundledCliFromBaseName(basename(normalized).toLowerCase())
}

function aicliFromAnyCommand(command: string): BundledCli | null {
  const normalized = normalizeCliCommand(command)
  return bundledCliFromBaseName(basename(normalized).toLowerCase())
}

function bundledCliLabel(tool: BundledCli): BundledCliLabel {
  return BUNDLED_CLIS.find((entry) => entry.id === tool)!.label
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
    // "File not found: ...\\app.asar\\bin\\aicli\\...\\codex.exe".
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
  // 内置 AICLI（codex）时非空；claude 及其它命令为 null（不受内置约束）。
  tool: BundledCli | null
  label: BundledCliLabel | null
  // 内置二进制的绝对路径；解析到内置时非空。
  bundledCommand: string | null
  // 是内置 AICLI 但没找到二进制：调用方必须报错，绝不回退宿主机版本。
  bundledMissing: boolean
}

// 内置 AICLI 已随应用深度定制，策略：只允许运行随应用打包的「内置」版本。
// 无论配置成裸命令还是宿主机上的 PATH / 自定义路径，一律解析到内置二进制；
// 解析不到就置 bundledMissing（调用方据此报错，绝不回退到宿主机自行安装的版本）。
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

// 内置 AICLI 必须用内置版本时，找不到内置二进制的统一报错文案。
export function bundledCliMissingMessage(resolution: AicliCommandResolution): string {
  const label = resolution.label ?? 'AICLI'
  const tool = resolution.tool ?? ''
  const family = BUNDLED_CLIS.map((entry) => entry.id).join(' / ')
  return (
    `未找到内置的 ${label} 可执行文件。${label}（${family}）已随应用深度定制，` +
    `仅支持随应用打包的内置版本，无法使用宿主机上自行安装的 ${tool}。` +
    `请重新安装应用，或在源码仓库执行 \`npm run build:aicli\` 生成 bin/aicli 下的二进制。`
  )
}
