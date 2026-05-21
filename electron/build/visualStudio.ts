import { execFile } from 'child_process'
import { join } from 'path'
import { promisify } from 'util'

const execFileAsync = promisify(execFile)
const DEFAULT_VSWHERE_PATH = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe'

export interface VisualStudioInstallation {
  instanceId: string
  displayName: string
  installationPath: string
  productLineVersion: string | null
  isPrerelease: boolean
}

export interface VisualStudioInstallationsResultOk {
  ok: true
  value: VisualStudioInstallation[]
}

export interface VisualStudioInstallationsResultError {
  ok: false
  error: string
}

export type VisualStudioInstallationsResult =
  | VisualStudioInstallationsResultOk
  | VisualStudioInstallationsResultError

export interface VisualStudioEnvironmentResultOk {
  ok: true
  displayName: string
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

function normalizeVisualStudioInstallation(value: unknown): VisualStudioInstallation | null {
  if (!value || typeof value !== 'object') return null

  const record = value as Record<string, unknown>
  const installationPath =
    typeof record.installationPath === 'string' ? record.installationPath.trim() : ''
  if (!installationPath) return null

  const instanceId = typeof record.instanceId === 'string' ? record.instanceId.trim() : ''
  const displayName = typeof record.displayName === 'string' ? record.displayName : installationPath
  const productLineVersion =
    typeof record.productLineVersion === 'string'
      ? record.productLineVersion
      : record.catalog &&
          typeof record.catalog === 'object' &&
          typeof (record.catalog as Record<string, unknown>).productLineVersion === 'string'
        ? ((record.catalog as Record<string, unknown>).productLineVersion as string)
        : null
  const isPrerelease = record.isPrerelease === true

  return {
    instanceId,
    displayName,
    installationPath,
    productLineVersion,
    isPrerelease,
  }
}

export async function listVisualStudioInstallations(options?: {
  platform?: NodeJS.Platform
  baseEnv?: NodeJS.ProcessEnv
  execFile?: ExecFileLike
  vswherePath?: string
}): Promise<VisualStudioInstallationsResult> {
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
      [
        '-nologo',
        '-utf8',
        '-prerelease',
        '-products',
        '*',
        '-requires',
        'Microsoft.Component.MSBuild',
        '-format',
        'json',
      ],
      { env: baseEnv, timeout: 5000, maxBuffer: 1024 * 1024 }
    )

    const parsed = JSON.parse(vswhereStdout) as unknown
    if (!Array.isArray(parsed)) {
      return { ok: false, error: 'vswhere output was not an array' }
    }
    const rawItems = parsed
    const value = rawItems
      .map((item) => normalizeVisualStudioInstallation(item))
      .filter((item): item is VisualStudioInstallation => item !== null)

    return { ok: true, value }
  } catch (error: unknown) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}

export async function resolveVisualStudioEnvironment(options: {
  instanceId: string
  platform?: NodeJS.Platform
  baseEnv?: NodeJS.ProcessEnv
  execFile?: ExecFileLike
  vswherePath?: string
}): Promise<VisualStudioEnvironmentResult> {
  const installationsResult = await listVisualStudioInstallations(options)
  if (!installationsResult.ok) {
    return installationsResult
  }

  const installation = installationsResult.value.find(
    (item) => item.instanceId === options.instanceId
  )
  if (!installation) {
    return { ok: false, error: `visual studio instance not found: ${options.instanceId}` }
  }

  const baseEnv = copyDefinedEnv(options.baseEnv ?? process.env)
  const runExecFile: ExecFileLike =
    options.execFile ??
    ((file, args, execOptions) =>
      execFileAsync(file, args, execOptions) as Promise<{ stdout: string; stderr: string }>)

  try {
    const devCmdPath = join(installation.installationPath, 'Common7', 'Tools', 'VsDevCmd.bat')
    const { stdout } = await runExecFile('cmd.exe', ['/d', '/s', '/c', `"${devCmdPath}" -no_logo && set`], {
      env: baseEnv,
      timeout: 15000,
      maxBuffer: 8 * 1024 * 1024,
    })

    return {
      ok: true,
      displayName: installation.displayName,
      installationPath: installation.installationPath,
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
