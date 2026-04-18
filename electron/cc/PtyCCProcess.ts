import { spawn, IPty } from 'node-pty'
import { EventEmitter } from 'events'
import { delimiter, join, isAbsolute, extname } from 'path'
import { statSync, readFileSync, writeFileSync } from 'fs'

const isWindows = process.platform === 'win32'

/** Resolve a bare command name against PATH + PATHEXT. Returns full path or null. */
function resolveOnPath(cmd: string, envPath: string): string | null {
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
    // On Windows, only match files with a valid PATHEXT extension — a bare
    // name often collides with an sh script (e.g. npm installs `claude` as
    // both a POSIX shim and `claude.cmd`; spawning the shim via CreateProcess
    // fails with ERROR_FILE_NOT_FOUND).
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

export interface PtyCCOptions {
  /** CC CLI binary path; defaults to 'claude' from PATH */
  command?: string
  /** Extra args passed to the CLI */
  args?: string[]
  /** Working directory the CC process runs in */
  cwd: string
  /** Initial terminal size */
  cols?: number
  rows?: number
  /** Extra environment overrides */
  env?: Record<string, string>
  /**
   * Windows only. When true, the MSYS-related env purge is skipped and the
   * MSYS usr/bin dir (if provided) is prepended to PATH, so .sh scripts
   * spawned by the CLI can resolve bash / unix tools. Default false to keep
   * existing behavior.
   */
  enableMsys?: boolean
  /** Absolute path to MSYS bash.exe (required when enableMsys is true). */
  msysBashPath?: string
  /** Dir containing MSYS bash.exe and unix coreutils. */
  msysUsrBinDir?: string
}

/**
 * Wraps a single `claude` CLI subprocess running inside a PTY.
 * Emits 'data' (string chunks), 'exit' ({exitCode, signal}).
 */
export class PtyCCProcess extends EventEmitter {
  private pty: IPty | null = null
  private readonly opts: PtyCCOptions

  constructor(opts: PtyCCOptions) {
    super()
    this.opts = opts
  }

  start(): void {
    if (this.pty) return

    const command = this.opts.command ?? 'claude'
    const args = this.opts.args ?? []

    const env: Record<string, string> = {
      ...(process.env as Record<string, string>),
      ...(this.opts.env ?? {}),
      TERM: 'xterm-256color',
      COLORTERM: 'truecolor'
    }

    // Suppress Codex CLI upgrade prompts by dismissing the latest version.
    // version.json lives in ~/.codex/; we overwrite dismissed_version on
    // every spawn so codex never pauses to show the "update available" banner.
    if (command === 'codex' || command.endsWith('/codex') || command.endsWith('\\codex')) {
      try {
        const codexDir = isWindows
          ? join(process.env.USERPROFILE ?? '', '.codex')
          : join(process.env.HOME ?? '', '.codex')
        const versionFile = join(codexDir, 'version.json')
        const raw = JSON.parse(readFileSync(versionFile, 'utf8'))
        if (raw.latest_version && raw.dismissed_version !== raw.latest_version) {
          raw.dismissed_version = raw.latest_version
          writeFileSync(versionFile, JSON.stringify(raw), 'utf8')
        }
      } catch {
        /* ignore — file may not exist on first run */
      }
    }

    // On Windows, when Electron is launched from Git Bash / MSYS, HOME is a
    // POSIX path like "/c/Users/Administrator" — and any CLI that reads
    // $HOME (claude, codex, etc.) will fail to find its credentials under
    // that path on a native NTFS filesystem, producing OAuth 403 / "not
    // logged in" symptoms even though the user IS logged in via cmd.
    // Force HOME to the Windows-style USERPROFILE so credential lookups
    // resolve to the real %USERPROFILE%\.claude\ etc.
    if (isWindows && env.USERPROFILE) {
      env.HOME = env.USERPROFILE
    }

    // CRITICAL: strip CLAUDE_CODE_* / CLAUDECODE* env vars before spawning.
    // If Claude Code (the CLI) happens to be our ancestor process — e.g.
    // when the user runs `npm run dev` from a terminal Claude Code spawned —
    // it injects CLAUDE_CODE_ENTRYPOINT / CLAUDE_CODE_EXECPATH / CLAUDECODE
    // into our env. Passing those through to a nested `claude` spawn makes
    // the nested instance believe it's running as an SDK/MCP subagent of
    // a host Claude Code, which disables its own OAuth flow and
    // credentials-manager lookups — `/login` will complete in the browser
    // but the returned token is never used, producing persistent
    // "OAuth 403 / Request not allowed" even for a logged-in user.
    for (const k of Object.keys(env)) {
      if (/^CLAUDE_?CODE(_|$)/i.test(k)) delete env[k]
    }

    // On Windows, purge variables from MSYS2 / Git-for-Windows that confuse
    // child processes spawned by the AI CLI (bash.exe, MSBuild, etc.).
    // Skipped when `enableMsys` is true — the caller explicitly wants .sh
    // scripts to resolve to MSYS bash, so we keep MSYSTEM/SHELL etc.
    if (isWindows && !this.opts.enableMsys) {
      const toxic = [
        'MSYSTEM', 'MSYSTEM_CARCH', 'MSYSTEM_CHOST', 'MSYSTEM_PREFIX',
        'MSYS', 'CHERE_INVOKING', 'SHELL', 'CONFIG_SITE', 'ORIGINAL_PATH',
        'MINGW_CHOST', 'MINGW_PREFIX', 'MINGW_PACKAGE_PREFIX',
        'PKG_CONFIG_PATH', 'PKG_CONFIG_SYSTEM_INCLUDE_PATH',
        'PKG_CONFIG_SYSTEM_LIBRARY_PATH', 'ACLOCAL_PATH',
        // Prevents cmd.exe / MSBuild from finding executables in cwd
        'NoDefaultCurrentDirectoryInExePath'
      ]
      for (const key of toxic) {
        delete env[key]
        // env keys may differ in case on Windows; do a case-insensitive sweep
        for (const k of Object.keys(env)) {
          if (k.toLowerCase() === key.toLowerCase()) delete env[k]
        }
      }
    }

    // When MSYS is explicitly enabled, ensure usr/bin and bash are wired in.
    if (isWindows && this.opts.enableMsys && this.opts.msysBashPath) {
      env.SHELL = this.opts.msysBashPath.replace(/\\/g, '/')
      if (!env.MSYSTEM) env.MSYSTEM = 'MINGW64'
    }

    // Ensure common install dirs are in PATH (Electron may launch without the
    // user's shell rc loaded). Use platform-correct delimiter.
    const extraPaths = isWindows
      ? [
          // MSYS usr/bin goes first when enabled, so `bash` / coreutils resolve there.
          ...(this.opts.enableMsys && this.opts.msysUsrBinDir ? [this.opts.msysUsrBinDir] : []),
          join(process.env.APPDATA ?? '', 'npm'),
          join(process.env.LOCALAPPDATA ?? '', 'npm'),
          join(process.env.USERPROFILE ?? '', '.local', 'bin')
        ].filter(Boolean)
      : ['/opt/homebrew/bin', '/usr/local/bin']
    const pathKey = isWindows
      ? Object.keys(env).find((k) => k.toLowerCase() === 'path') ?? 'Path'
      : 'PATH'
    const currentPath = env[pathKey] ?? ''
    const parts = currentPath.split(delimiter)
    for (const p of extraPaths) {
      if (p && !parts.includes(p)) parts.unshift(p)
    }
    env[pathKey] = parts.filter(Boolean).join(delimiter)

    // On Windows, node-pty calls CreateProcessW directly — it won't resolve
    // bare names ("claude") or `.cmd` shims. Resolve against PATH+PATHEXT,
    // and wrap `.cmd`/`.bat` via cmd.exe.
    let spawnCommand = command
    let spawnArgs = args
    if (isWindows) {
      const resolved = resolveOnPath(command, env[pathKey] ?? '')
      if (!resolved) {
        throw new Error(
          `找不到可执行文件: ${command}。请确认已安装并在 PATH 中（当前 PATH: ${env[pathKey]}）`
        )
      }
      const ext = extname(resolved).toLowerCase()
      if (ext === '.cmd' || ext === '.bat') {
        spawnCommand = process.env.ComSpec ?? 'cmd.exe'
        spawnArgs = ['/d', '/s', '/c', resolved, ...args]
      } else {
        spawnCommand = resolved
      }
    }

    // Debug: on MULTI_AI_CODE_PTY_DUMP=1, dump a summary of the env we're
    // about to hand to the CLI. Values of credential-like vars are redacted
    // (length + first 2 chars) so we can diagnose auth issues without leaking
    // secrets.
    if (process.env.MULTI_AI_CODE_PTY_DUMP === '1') {
      const SENSITIVE = /^(ANTHROPIC_|CLAUDE_|OPENAI_|.*_API_KEY|.*_TOKEN|.*_SECRET)/i
      const interesting = [
        'HOME', 'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH',
        'APPDATA', 'LOCALAPPDATA', 'TEMP', 'TMP',
        'SHELL', 'MSYSTEM',
        'HTTP_PROXY', 'HTTPS_PROXY', 'NO_PROXY',
        'http_proxy', 'https_proxy', 'no_proxy',
        'NODE_EXTRA_CA_CERTS', 'NODE_TLS_REJECT_UNAUTHORIZED',
        'SSL_CERT_FILE', 'SSL_CERT_DIR',
        'ELECTRON_RUN_AS_NODE'
      ]
      const summary: Record<string, string> = {}
      for (const k of interesting) {
        if (env[k] !== undefined) summary[k] = env[k]
      }
      for (const k of Object.keys(env)) {
        if (SENSITIVE.test(k)) {
          const v = env[k] ?? ''
          summary[k] = `<redacted len=${v.length} head="${v.slice(0, 2)}">`
        }
      }
      summary['__command'] = spawnCommand
      summary['__args'] = JSON.stringify(spawnArgs)
      summary['__cwd'] = this.opts.cwd
      summary['__pathHead'] = (env[pathKey] ?? '').split(delimiter).slice(0, 6).join(' | ')
      // Full env key list (no values) — used to find vars not covered by the
      // interesting/sensitive whitelists, e.g. bash-injected vars that may
      // affect nested `claude` auth.
      summary['__allKeys'] = Object.keys(env).sort().join(',')
      console.log('[pty-dump][env]', JSON.stringify(summary, null, 2))
    }

    this.pty = spawn(spawnCommand, spawnArgs, {
      name: 'xterm-256color',
      cols: this.opts.cols ?? 100,
      rows: this.opts.rows ?? 30,
      cwd: this.opts.cwd,
      env
    })

    this.pty.onData((chunk) => this.emit('data', chunk))
    this.pty.onExit(({ exitCode, signal }) => {
      this.emit('exit', { exitCode, signal })
      this.pty = null
    })
  }

  write(data: string): void {
    this.pty?.write(data)
  }

  resize(cols: number, rows: number): void {
    if (cols > 0 && rows > 0) {
      try {
        this.pty?.resize(cols, rows)
      } catch {
        // ignore transient resize errors
      }
    }
  }

  kill(signal?: string): void {
    if (!this.pty) return
    try {
      this.pty.kill(signal)
    } catch {
      // process may already be gone
    }
    this.pty = null
  }

  get running(): boolean {
    return this.pty !== null
  }
}
