import { spawn, IPty } from 'node-pty'
import { EventEmitter } from 'events'
import { delimiter, join, isAbsolute, extname } from 'path'
import { statSync } from 'fs'

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

    // Ensure common install dirs are in PATH (Electron may launch without the
    // user's shell rc loaded). Use platform-correct delimiter.
    const extraPaths = isWindows
      ? [
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
