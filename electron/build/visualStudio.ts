import { execFile } from 'child_process'
import { join } from 'path'
import { promisify } from 'util'

const execFileAsync = promisify(execFile)
const DEFAULT_VSWHERE_PATH = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe'

export interface VisualStudioEnvironmentResultOk {
  ok: true
  installationPath: string
  devCmdPath: string
  env: NodeJS.ProcessEnv
}

export interface VisualStudioEnvironmentResultError {
  ok: false
  error: string
}

export type VisualStudioEnvironmentResult =
  | VisualStudioEnvironmentResultOk
  | VisualStudioEnvironmentResultError

export type ExecFileLike = (
  file: string,
  args: string[],
  options?: { cwd?: string; env?: NodeJS.ProcessEnv; timeout?: number; maxBuffer?: number }
) => Promise<{ stdout: string; stderr: string }>

function copyDefinedEnv(input: NodeJS.ProcessEnv): NodeJS.ProcessEnv {
  const output: NodeJS.ProcessEnv = {}
  for (const [key, value] of Object.entries(input)) {
    if (value !== undefined) output[key] = value
  }
  return output
}

export function parseVisualStudioEnvironmentBlock(raw: string): NodeJS.ProcessEnv {
  const env: NodeJS.ProcessEnv = {}
  for (const line of raw.split(/\r?\n/)) {
    const index = line.indexOf('=')
    if (index <= 0) continue
    env[line.slice(0, index)] = line.slice(index + 1)
  }
  return env
}

export async function resolveVisualStudioEnvironment(options?: {
  platform?: NodeJS.Platform
  baseEnv?: NodeJS.ProcessEnv
  execFile?: ExecFileLike
  vswherePath?: string
}): Promise<VisualStudioEnvironmentResult> {
  const platform = options?.platform ?? process.platform
  if (platform !== 'win32') {
    return { ok: false, error: 'visual-studio environment is only available on Windows' }
  }

  const baseEnv = copyDefinedEnv(options?.baseEnv ?? process.env)
  const runExecFile: ExecFileLike =
    options?.execFile ??
    ((file, args, execOptions) =>
      execFileAsync(file, args, execOptions) as Promise<{ stdout: string; stderr: string }>)

  try {
    const { stdout: vswhereStdout } = await runExecFile(
      options?.vswherePath ?? DEFAULT_VSWHERE_PATH,
      ['-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild', '-property', 'installationPath'],
      { env: baseEnv, timeout: 5000, maxBuffer: 1024 * 1024 }
    )

    const installationPath = vswhereStdout.trim()
    if (!installationPath) {
      return { ok: false, error: 'visual studio installation not found' }
    }

    const devCmdPath = join(installationPath, 'Common7', 'Tools', 'VsDevCmd.bat')
    const { stdout } = await runExecFile(
      'cmd.exe',
      ['/d', '/s', '/c', `"${devCmdPath}" -no_logo && set`],
      { env: baseEnv, timeout: 15000, maxBuffer: 8 * 1024 * 1024 }
    )

    return {
      ok: true,
      installationPath,
      devCmdPath,
      env: {
        ...baseEnv,
        ...parseVisualStudioEnvironmentBlock(stdout),
      },
    }
  } catch (error: unknown) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}
