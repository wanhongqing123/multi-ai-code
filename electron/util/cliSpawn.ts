import { basename, delimiter, dirname, extname, isAbsolute, join } from 'path'
import { statSync } from 'fs'
import {
  describeAicliLaunchCommand,
  resolveBundledCliCommand
} from '../aicli/bundledCliResolver.js'

/**
 * Platform-aware spawn helper for one-shot AI CLI invocations.
 *
 * Why this exists separately from electron/cc/PtyCCProcess.ts:
 *   - PtyCCProcess targets an interactive PTY (node-pty); it scrubs MSYS env
 *     vars and CLAUDE_CODE_* vars aggressively, which is necessary for a
 *     long-running TUI session. A *one-shot* subprocess with stdout capture
 *     and short timeout does not need the full TUI env, but does need
 *     correct binary resolution + a PATH augmented with the usual install
 *     locations (npm/.local/bin, brew).
 *   - Bundling all that into PtyCCProcess would either over-scrub our
 *     subprocess env or leak PTY-specific state into the generator.
 *
 * Behavior summary:
 *   - Windows: resolves bare names against PATH + PATHEXT, wraps .cmd/.bat
 *     via cmd.exe explicitly (no `shell: true`, no shell-injection risk).
 *   - macOS/Linux: spawns directly; PATH is augmented with /opt/homebrew/bin
 *     and /usr/local/bin so brew-installed CLIs resolve when Electron is
 *     launched without the user's shell rc loaded.
 */

const isWindows = process.platform === 'win32'

function normalizeCliCommand(cmd: string): string {
  let normalized = cmd.trim()
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

function fallbackCliAlias(cmd: string): 'claude' | 'codex' | null {
  const base = basename(cmd).toLowerCase()
  if (/^claude(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'claude'
  if (/^codex(\.(exe|cmd|bat|ps1))?$/.test(base)) return 'codex'
  return null
}

function isExistingFile(path: string): boolean {
  try {
    return statSync(path).isFile()
  } catch {
    return false
  }
}

function resolveClaudeNativeBinary(cmd: string): string | null {
  if (!isWindows) return null
  const normalized = normalizeCliCommand(cmd)
  const base = basename(normalized).toLowerCase()

  let packageRoot: string | null = null
  if (base === 'claude' || base === 'claude.cmd' || base === 'claude.ps1') {
    packageRoot = join(dirname(normalized), 'node_modules', '@anthropic-ai', 'claude-code')
  } else if (base === 'claude.exe') {
    const binDir = dirname(normalized)
    const pkgRoot = dirname(binDir)
    if (
      basename(binDir).toLowerCase() === 'bin' &&
      basename(pkgRoot).toLowerCase() === 'claude-code' &&
      basename(dirname(pkgRoot)).toLowerCase() === '@anthropic-ai'
    ) {
      packageRoot = pkgRoot
    }
  }

  if (!packageRoot) return null

  const directBin = join(packageRoot, 'bin', 'claude.exe')
  if (isExistingFile(directBin)) return directBin

  const nativePkg =
    process.arch === 'arm64' ? 'claude-code-win32-arm64' : 'claude-code-win32-x64'
  const nestedNative = join(
    packageRoot,
    'node_modules',
    '@anthropic-ai',
    nativePkg,
    'claude.exe'
  )
  return isExistingFile(nestedNative) ? nestedNative : null
}

export function resolveOnPath(cmd: string, envPath: string): string | null {
  const normalized = normalizeCliCommand(cmd)
  if (isAbsolute(normalized) || normalized.includes('/') || normalized.includes('\\')) {
    const nativeClaude = resolveClaudeNativeBinary(normalized)
    if (nativeClaude) return nativeClaude
    if (isExistingFile(normalized)) return normalized
    const alias = fallbackCliAlias(normalized)
    if (alias) return resolveOnPath(alias, envPath)
    return null
  }
  const exts = isWindows
    ? (process.env.PATHEXT ?? '.COM;.EXE;.BAT;.CMD').split(';').map((e) => e.toLowerCase())
    : ['']
  const dirs = envPath.split(delimiter).filter(Boolean)
  const hasExt = isWindows && exts.includes(extname(normalized).toLowerCase())
  for (const dir of dirs) {
    const candidates = hasExt
      ? [normalized]
      : isWindows
        ? exts.map((e) => normalized + e)
        : [normalized]
    for (const name of candidates) {
      const full = join(dir, name)
      if (isExistingFile(full)) {
        return resolveClaudeNativeBinary(full) ?? full
      }
    }
  }
  return null
}

/**
 * Augments PATH with the directories Electron typically misses when
 * launched outside a user shell. Returns a new env object; the input is
 * not mutated.
 */
export function buildEnvWithPath(
  baseEnv: Record<string, string | undefined>
): Record<string, string> {
  const env: Record<string, string> = {}
  for (const [k, v] of Object.entries(baseEnv)) {
    if (typeof v === 'string') env[k] = v
  }
  const extraPaths = isWindows
    ? [
        join(env.APPDATA ?? '', 'npm'),
        join(env.LOCALAPPDATA ?? '', 'npm'),
        join(env.USERPROFILE ?? '', '.local', 'bin')
      ].filter((p) => p && p.length > 1)
    : [
        '/opt/homebrew/bin',
        '/usr/local/bin',
        join(env.HOME ?? '', '.local', 'bin')
      ].filter((p) => p && p.length > 1)
  const pathKey = isWindows
    ? Object.keys(env).find((k) => k.toLowerCase() === 'path') ?? 'Path'
    : 'PATH'
  const current = env[pathKey] ?? ''
  const parts = current.split(delimiter)
  for (const p of extraPaths) {
    if (!parts.includes(p)) parts.unshift(p)
  }
  env[pathKey] = parts.filter(Boolean).join(delimiter)
  // On Windows, when Electron is launched from Git Bash, HOME is POSIX-style
  // and breaks credential lookups for claude/codex. Mirror PtyCCProcess.
  if (isWindows && env.USERPROFILE) {
    env.HOME = env.USERPROFILE
  }
  return env
}

export interface ResolvedSpawn {
  /** Final executable. On Windows .cmd/.bat targets this is cmd.exe. */
  spawnCommand: string
  /** Final args. On Windows .cmd/.bat targets, prefixed with /d /s /c <resolved>. */
  spawnArgs: string[]
  /** Whether to use `shell: true`. Always false here — we resolve explicitly. */
  shell: false
  /** User-visible diagnostics for Codex/OpenCode launch source. */
  launchNotice?: string
}

export interface ResolveSpawnResult {
  ok: true
  resolved: ResolvedSpawn
}

export interface ResolveSpawnError {
  ok: false
  error: string
}

/**
 * Resolves a one-shot CLI invocation into the actual spawn arguments. Pure
 * function (apart from filesystem stat in resolveOnPath); fully testable on
 * either platform.
 */
export function resolveCliSpawn(
  command: string,
  args: string[],
  env: Record<string, string>
): ResolveSpawnResult | ResolveSpawnError {
  const normalizedCommand = normalizeCliCommand(command)
  const isCustomCommand =
    isAbsolute(normalizedCommand) ||
    normalizedCommand.includes('/') ||
    normalizedCommand.includes('\\')
  const bundledCommand = resolveBundledCliCommand(normalizedCommand)
  const resolvedBundledCommand = bundledCommand ?? normalizedCommand
  const pathKey = Object.keys(env).find((k) => k.toLowerCase() === 'path') ?? 'Path'
  const envPath = env[pathKey] ?? ''
  if (!isWindows) {
    const displayCommand =
      bundledCommand ??
      (isCustomCommand ? resolvedBundledCommand : resolveOnPath(normalizedCommand, envPath)) ??
      resolvedBundledCommand
    const launchNotice = describeAicliLaunchCommand(
      normalizedCommand,
      displayCommand,
      bundledCommand
    )?.notice
    return {
      ok: true,
      resolved: {
        spawnCommand: resolvedBundledCommand,
        spawnArgs: args,
        shell: false,
        launchNotice
      }
    }
  }
  const resolved = resolveOnPath(resolvedBundledCommand, envPath)
  if (!resolved) {
    return {
      ok: false,
      error: `找不到可执行文件: ${command}（PATH 中没有匹配项）`
    }
  }
  const launchNotice = describeAicliLaunchCommand(normalizedCommand, resolved, bundledCommand)?.notice
  const ext = extname(resolved).toLowerCase()
  if (ext === '.cmd' || ext === '.bat') {
    return {
      ok: true,
      resolved: {
        spawnCommand: process.env.ComSpec ?? 'cmd.exe',
        spawnArgs: ['/d', '/s', '/c', resolved, ...args],
        shell: false,
        launchNotice
      }
    }
  }
  return {
    ok: true,
    resolved: {
      spawnCommand: resolved,
      spawnArgs: args,
      shell: false,
      launchNotice
    }
  }
}
