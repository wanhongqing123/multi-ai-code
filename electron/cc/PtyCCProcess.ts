import { spawn, IPty } from 'node-pty'
import { EventEmitter } from 'events'

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
    // user's shell rc loaded)
    const extraPaths = ['/opt/homebrew/bin', '/usr/local/bin']
    const currentPath = env.PATH ?? ''
    for (const p of extraPaths) {
      if (!currentPath.split(':').includes(p)) {
        env.PATH = `${p}:${currentPath}`
      }
    }

    this.pty = spawn(command, args, {
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
