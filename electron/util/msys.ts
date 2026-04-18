import { promises as fs } from 'fs'
import { dirname } from 'path'
import { execFile } from 'child_process'
import { promisify } from 'util'

const execFileAsync = promisify(execFile)

export interface MsysInfo {
  available: boolean
  bashPath: string | null
  usrBinDir: string | null
  variant: 'msys2' | 'git' | 'path' | null
  /** Candidates probed (for UI debugging / doctor display). */
  candidates: { path: string; exists: boolean; variant: 'msys2' | 'git' }[]
}

const CANDIDATES: { path: string; variant: 'msys2' | 'git' }[] = [
  // User-requested priority: MSYS2 first.
  { path: 'C:\\msys64\\usr\\bin\\bash.exe', variant: 'msys2' },
  { path: 'C:\\Program Files\\Git\\usr\\bin\\bash.exe', variant: 'git' },
  { path: 'C:\\Program Files\\Git\\bin\\bash.exe', variant: 'git' },
  { path: 'C:\\Program Files (x86)\\Git\\usr\\bin\\bash.exe', variant: 'git' }
]

export async function detectMsys(): Promise<MsysInfo> {
  if (process.platform !== 'win32') {
    return { available: false, bashPath: null, usrBinDir: null, variant: null, candidates: [] }
  }
  const probes = await Promise.all(
    CANDIDATES.map(async (c) => {
      try {
        await fs.access(c.path)
        return { ...c, exists: true }
      } catch {
        return { ...c, exists: false }
      }
    })
  )
  const hit = probes.find((p) => p.exists)
  if (hit) {
    return {
      available: true,
      bashPath: hit.path,
      usrBinDir: dirname(hit.path),
      variant: hit.variant,
      candidates: probes
    }
  }
  // Fall back to PATH lookup.
  try {
    const { stdout } = await execFileAsync('where', ['bash'], { timeout: 2000 })
    const first = stdout.trim().split(/\r?\n/)[0]
    if (first && first.toLowerCase().endsWith('bash.exe')) {
      return {
        available: true,
        bashPath: first,
        usrBinDir: dirname(first),
        variant: 'path',
        candidates: probes
      }
    }
  } catch {
    /* bash not on PATH */
  }
  return { available: false, bashPath: null, usrBinDir: null, variant: null, candidates: probes }
}

/**
 * Command/args tuple that launches the matching shell window with a specific
 * cwd. Returns null on non-Windows or when no shell is available.
 */
export function buildOpenMsysTerminalCommand(
  info: MsysInfo,
  cwd: string
): { command: string; args: string[] } | null {
  if (!info.available) return null
  if (info.variant === 'msys2') {
    // msys2_shell.cmd is next to bash at <msys64>/msys2_shell.cmd
    const root = dirname(dirname(info.usrBinDir ?? ''))
    const shellCmd = `${root}\\msys2_shell.cmd`
    // -defterm: pick default terminal; -mingw64: MINGW64 subsystem; -here: cwd
    return {
      command: 'cmd.exe',
      args: ['/c', 'start', '""', shellCmd, '-defterm', '-mingw64', '-no-start', '-here']
    }
  }
  // Git-for-Windows / PATH: launch git-bash.exe if present, else plain bash.exe
  const gitBashGuess = (info.usrBinDir ?? '').replace(/\\usr\\bin$/i, '\\git-bash.exe')
  return {
    command: 'cmd.exe',
    args: ['/c', 'start', '""', gitBashGuess, `--cd=${cwd}`]
  }
}
