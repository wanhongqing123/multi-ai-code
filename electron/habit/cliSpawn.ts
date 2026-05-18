import { delimiter, extname, isAbsolute, join } from 'path'
import { statSync } from 'fs'

/**
 * Platform-aware spawn helper for one-shot AI CLI invocations from the
 * habit-learning generator.
 *
 * Why this exists separately from electron/cc/PtyCCProcess.ts:
 *   - PtyCCProcess targets an interactive PTY (node-pty); it scrubs MSYS env
 *     vars and CLAUDE_CODE_* vars aggressively, which is necessary for a
 *     long-running TUI session. The habit generator runs a *one-shot*
 *     subprocess with stdout capture and short timeout — it does not need
 *     the full TUI env, but does need correct binary resolution + a PATH
 *     augmented with the usual install locations (npm/.local/bin, brew).
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

export function resolveOnPath(cmd: string, envPath: string): string | null {
  if (isAbsolute(cmd) || cmd.includes('/') || cmd.includes('\\')) {
    try {
      if (statSync(cmd).isFile()) return cmd
    } catch {
      /* fall through */
    }
    return null
  }
  const exts = isWindows
    ? (process.env.PATHEXT ?? '.COM;.EXE;.BAT;.CMD').split(';').map((e) => e.toLowerCase())
    : ['']
  const dirs = envPath.split(delimiter).filter(Boolean)
  const hasExt = isWindows && exts.includes(extname(cmd).toLowerCase())
  for (const dir of dirs) {
    const candidates = hasExt ? [cmd] : isWindows ? exts.map((e) => cmd + e) : [cmd]
    for (const name of candidates) {
      const full = join(dir, name)
      try {
        if (statSync(full).isFile()) return full
      } catch {
        /* not found */
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
  if (!isWindows) {
    return {
      ok: true,
      resolved: {
        spawnCommand: command,
        spawnArgs: args,
        shell: false
      }
    }
  }
  const pathKey = Object.keys(env).find((k) => k.toLowerCase() === 'path') ?? 'Path'
  const envPath = env[pathKey] ?? ''
  const resolved = resolveOnPath(command, envPath)
  if (!resolved) {
    return {
      ok: false,
      error: `找不到可执行文件: ${command}（PATH 中没有匹配项）`
    }
  }
  const ext = extname(resolved).toLowerCase()
  if (ext === '.cmd' || ext === '.bat') {
    return {
      ok: true,
      resolved: {
        spawnCommand: process.env.ComSpec ?? 'cmd.exe',
        spawnArgs: ['/d', '/s', '/c', resolved, ...args],
        shell: false
      }
    }
  }
  return {
    ok: true,
    resolved: {
      spawnCommand: resolved,
      spawnArgs: args,
      shell: false
    }
  }
}
