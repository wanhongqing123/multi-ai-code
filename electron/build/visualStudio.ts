import { execFile, spawn } from 'child_process'
import iconv from 'iconv-lite'
import { win32 } from 'path'
import type { VisualStudioInstallation } from './types.js'

export type { VisualStudioInstallation }

const DEFAULT_VSWHERE_PATH = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe'

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

interface ExecFileResult {
  stdout: string | Buffer
  stderr: string | Buffer
}

export type ExecFileLike = (
  file: string,
  args: string[],
  options?: { cwd?: string; env?: NodeJS.ProcessEnv; timeout?: number; maxBuffer?: number }
) => Promise<ExecFileResult>

type ExecFileError = Error & {
  stdout?: string | Buffer
  stderr?: string | Buffer
}

export type SpawnLike = typeof spawn

function defaultExecFile(
  file: string,
  args: string[],
  options?: { cwd?: string; env?: NodeJS.ProcessEnv; timeout?: number; maxBuffer?: number }
): Promise<ExecFileResult> {
  return new Promise((resolve, reject) => {
    execFile(
      file,
      args,
      {
        ...options,
        encoding: 'buffer',
      },
      (error, stdout, stderr) => {
        if (error) {
          const nextError = error as ExecFileError
          nextError.stdout = stdout
          nextError.stderr = stderr
          reject(nextError)
          return
        }
        resolve({ stdout, stderr })
      }
    )
  })
}

function decodeOutput(value: string | Buffer | undefined, encoding: 'utf8' | 'gbk'): string {
  if (value === undefined) return ''
  if (typeof value === 'string') return value
  return iconv.decode(value, encoding)
}

function formatExecFileError(error: unknown, encoding: 'utf8' | 'gbk'): string {
  if (!(error instanceof Error)) return String(error)

  const stderr = decodeOutput((error as ExecFileError).stderr, encoding).trim()
  if (stderr) return stderr

  const stdout = decodeOutput((error as ExecFileError).stdout, encoding).trim()
  if (stdout) return stdout

  return error.message
}

export async function runWindowsCommandVerbatim(
  command: string,
  options?: {
    cwd?: string
    env?: NodeJS.ProcessEnv
    timeout?: number
    maxBuffer?: number
    spawn?: SpawnLike
  }
): Promise<ExecFileResult> {
  const runSpawn = options?.spawn ?? spawn
  const timeout = options?.timeout ?? 0
  const maxBuffer = options?.maxBuffer ?? 1024 * 1024

  return new Promise((resolve, reject) => {
    const child = runSpawn('cmd.exe', ['/d', '/s', '/c', command], {
      cwd: options?.cwd,
      env: options?.env,
      windowsHide: true,
      windowsVerbatimArguments: true,
      stdio: ['ignore', 'pipe', 'pipe'],
    })

    let stdoutLength = 0
    let stderrLength = 0
    const stdoutChunks: Buffer[] = []
    const stderrChunks: Buffer[] = []
    let settled = false
    let timeoutHandle: NodeJS.Timeout | null = null

    const cleanup = () => {
      if (timeoutHandle) clearTimeout(timeoutHandle)
    }

    const rejectWithBuffers = (error: Error) => {
      if (settled) return
      settled = true
      cleanup()
      const nextError = error as ExecFileError
      nextError.stdout = Buffer.concat(stdoutChunks)
      nextError.stderr = Buffer.concat(stderrChunks)
      reject(nextError)
    }

    const appendChunk = (target: Buffer[], chunk: Buffer, stream: 'stdout' | 'stderr') => {
      target.push(chunk)
      if (stream === 'stdout') stdoutLength += chunk.length
      else stderrLength += chunk.length
      if (stdoutLength > maxBuffer || stderrLength > maxBuffer) {
        child.kill()
        rejectWithBuffers(new Error(`Command failed: ${stream} maxBuffer exceeded`))
      }
    }

    child.stdout?.on('data', (chunk: Buffer | string) => {
      appendChunk(stdoutChunks, Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk), 'stdout')
    })
    child.stderr?.on('data', (chunk: Buffer | string) => {
      appendChunk(stderrChunks, Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk), 'stderr')
    })
    child.on('error', (error) => {
      rejectWithBuffers(error)
    })
    child.on('close', (code, signal) => {
      if (settled) return
      settled = true
      cleanup()
      const stdout = Buffer.concat(stdoutChunks)
      const stderr = Buffer.concat(stderrChunks)
      if (code === 0) {
        resolve({ stdout, stderr })
        return
      }
      const error = new Error(
        `Command failed: cmd.exe /d /s /c ${command}${signal ? ` (signal: ${signal})` : ''}`
      ) as ExecFileError
      error.stdout = stdout
      error.stderr = stderr
      reject(error)
    })

    if (timeout > 0) {
      timeoutHandle = setTimeout(() => {
        child.kill()
        rejectWithBuffers(new Error(`Command failed: timed out after ${timeout}ms`))
      }, timeout)
    }
  })
}

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
  const runExecFile: ExecFileLike = options?.execFile ?? defaultExecFile

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

    const parsed = JSON.parse(decodeOutput(vswhereStdout, 'utf8')) as unknown
    if (!Array.isArray(parsed)) {
      return { ok: false, error: 'vswhere output was not an array' }
    }
    const rawItems = parsed
    const value = rawItems
      .map((item) => normalizeVisualStudioInstallation(item))
      .filter((item): item is VisualStudioInstallation => item !== null)

    return { ok: true, value }
  } catch (error: unknown) {
    return { ok: false, error: formatExecFileError(error, 'utf8') }
  }
}

export async function resolveVisualStudioEnvironment(options: {
  instanceId: string
  platform?: NodeJS.Platform
  baseEnv?: NodeJS.ProcessEnv
  execFile?: ExecFileLike
  spawn?: SpawnLike
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
  try {
    const devCmdPath = win32.join(
      installation.installationPath,
      'Common7',
      'Tools',
      'VsDevCmd.bat'
    )
    const { stdout } = await runWindowsCommandVerbatim(
      `call "${devCmdPath}" -no_logo && set`,
      {
        spawn: options.spawn,
        env: baseEnv,
        timeout: 15000,
        maxBuffer: 8 * 1024 * 1024,
      }
    )

    const decodedStdout = decodeOutput(stdout, 'gbk')

    return {
      ok: true,
      displayName: installation.displayName,
      installationPath: installation.installationPath,
      devCmdPath,
      env: {
        ...baseEnv,
        ...parseVisualStudioEnvironmentBlock(decodedStdout),
      },
    }
  } catch (error: unknown) {
    return { ok: false, error: formatExecFileError(error, 'gbk') }
  }
}
