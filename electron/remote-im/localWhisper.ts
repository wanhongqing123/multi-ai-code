import { exec as nodeExec, execFile as nodeExecFile } from 'child_process'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { basename, dirname, extname, join } from 'path'
import { fileURLToPath } from 'url'
import type { RemoteImIncomingAudioMessage } from './types.js'

export interface LocalWhisperSettings {
  command: string
  commandTemplate: string
  modelPath: string
  language: string
  timeoutMs: number
  ffmpegCommand: string
}

export type LocalWhisperCommand =
  | { mode: 'execFile'; command: string; args: string[] }
  | { mode: 'shell'; command: string }

export type LocalWhisperResult =
  | { ok: true; text: string }
  | { ok: false; error: string }

export interface LocalWhisperDeps {
  env?: NodeJS.ProcessEnv | Record<string, string | undefined>
  cwd?: string
  moduleUrl?: string
  modelSearchRoots?: string[]
  fileExists?: (path: string) => Promise<boolean>
  fetchArrayBuffer?: (url: string) => Promise<ArrayBuffer>
  runCommand?: (
    command: LocalWhisperCommand,
    timeoutMs: number
  ) => Promise<{ stdout: string; stderr: string; exitCode: number }>
  makeTempDir?: () => Promise<string>
  writeFile?: (path: string, data: Uint8Array) => Promise<void>
  readTextFileIfExists?: (path: string) => Promise<string | null>
  cleanupDir?: (path: string) => Promise<void>
}

function normalizeEnvString(value: unknown): string {
  return typeof value === 'string' ? value.trim() : ''
}

function normalizeEnvNumber(value: unknown, fallback: number): number {
  const parsed = Number(value)
  return Number.isFinite(parsed) && parsed > 0 ? Math.round(parsed) : fallback
}

export function readLocalWhisperSettings(
  env: NodeJS.ProcessEnv | Record<string, string | undefined> = process.env
): LocalWhisperSettings {
  return {
    command: normalizeEnvString(env.MULTI_AI_CODE_WHISPER_CMD),
    commandTemplate: normalizeEnvString(
      env.MULTI_AI_CODE_WHISPER_COMMAND ?? env.MULTI_AI_CODE_WHISPER_COMMAND_TEMPLATE
    ),
    modelPath: normalizeEnvString(env.MULTI_AI_CODE_WHISPER_MODEL),
    language: normalizeEnvString(env.MULTI_AI_CODE_WHISPER_LANGUAGE) || 'zh',
    timeoutMs: normalizeEnvNumber(env.MULTI_AI_CODE_WHISPER_TIMEOUT_MS, 120_000),
    ffmpegCommand: normalizeEnvString(env.MULTI_AI_CODE_FFMPEG_CMD)
  }
}

const DEFAULT_REPO_MODEL_NAMES = [
  'ggml-base.bin',
  'ggml-small.bin',
  'ggml-tiny.bin',
  'ggml-medium.bin'
]

const DEFAULT_WHISPER_CLI_CANDIDATES = [
  '/opt/homebrew/bin/whisper-cli',
  '/usr/local/bin/whisper-cli'
]

const DEFAULT_FFMPEG_CANDIDATES = [
  '/opt/homebrew/bin/ffmpeg',
  '/usr/local/bin/ffmpeg'
]

const WHISPER_CPP_SUPPORTED_EXTENSIONS = new Set(['.flac', '.mp3', '.ogg', '.wav'])

async function defaultFileExists(path: string): Promise<boolean> {
  try {
    await fs.access(path)
    return true
  } catch {
    return false
  }
}

async function firstExistingPath(
  paths: string[],
  deps: Pick<LocalWhisperDeps, 'fileExists'>
): Promise<string> {
  const fileExists = deps.fileExists ?? defaultFileExists
  for (const path of paths) {
    if (await fileExists(path)) return path
  }
  return ''
}

function uniqueStrings(values: string[]): string[] {
  return Array.from(new Set(values.filter(Boolean)))
}

function getAncestorDirectories(path: string, maxDepth: number): string[] {
  const dirs: string[] = []
  let current = path
  for (let index = 0; index < maxDepth; index += 1) {
    dirs.push(current)
    const next = dirname(current)
    if (next === current) break
    current = next
  }
  return dirs
}

function getModuleSearchRoots(moduleUrl: string): string[] {
  try {
    return getAncestorDirectories(dirname(fileURLToPath(moduleUrl)), 8)
  } catch {
    return []
  }
}

function getWhisperModelSearchRoots(input: {
  cwd: string
  env: NodeJS.ProcessEnv | Record<string, string | undefined>
  moduleUrl: string
  explicitRoots?: string[]
}): string[] {
  return uniqueStrings([
    ...(input.explicitRoots ?? []),
    normalizeEnvString(input.env.MULTI_AI_CODE_HOME),
    input.cwd,
    ...getModuleSearchRoots(input.moduleUrl)
  ])
}

function getRepositoryModelCandidates(roots: string[]): string[] {
  return roots.flatMap((root) =>
    DEFAULT_REPO_MODEL_NAMES.map((name) => join(root, 'models', 'whisper', name))
  )
}

async function resolveLocalWhisperSettings(
  settings: LocalWhisperSettings,
  deps: Pick<LocalWhisperDeps, 'cwd' | 'env' | 'fileExists' | 'modelSearchRoots' | 'moduleUrl'>
): Promise<LocalWhisperSettings> {
  const resolved = { ...settings }
  if (!resolved.modelPath) {
    resolved.modelPath = await firstExistingPath(
      getRepositoryModelCandidates(
        getWhisperModelSearchRoots({
          cwd: deps.cwd ?? process.cwd(),
          env: deps.env ?? process.env,
          moduleUrl: deps.moduleUrl ?? import.meta.url,
          explicitRoots: deps.modelSearchRoots
        })
      ),
      deps
    )
  }
  if (!resolved.command && !resolved.commandTemplate && resolved.modelPath) {
    resolved.command = await firstExistingPath(DEFAULT_WHISPER_CLI_CANDIDATES, deps)
  }
  if (!resolved.ffmpegCommand) {
    resolved.ffmpegCommand = await firstExistingPath(DEFAULT_FFMPEG_CANDIDATES, deps)
  }
  return resolved
}

function shellQuote(value: string): string {
  return `'${value.replace(/'/g, `'\\''`)}'`
}

function isWhisperCppCommand(command: string): boolean {
  const name = basename(command).toLowerCase()
  return name === 'whisper-cli' || name === 'main' || name.includes('whisper-cli')
}

export function buildLocalWhisperCommand(input: {
  settings: LocalWhisperSettings
  audioPath: string
  outputBasePath: string
}): LocalWhisperCommand {
  const outputTextPath = `${input.outputBasePath}.txt`
  if (input.settings.commandTemplate) {
    const replacements: Record<string, string> = {
      input: input.audioPath,
      output: outputTextPath,
      outputBase: input.outputBasePath,
      outputDir: dirname(input.outputBasePath),
      model: input.settings.modelPath,
      language: input.settings.language
    }
    let command = input.settings.commandTemplate
    for (const [key, value] of Object.entries(replacements)) {
      command = command.replaceAll(`{${key}}`, shellQuote(value))
    }
    return { mode: 'shell', command }
  }

  if (!input.settings.command) {
    throw new Error('本地 Whisper 未配置：请把模型放到 models/whisper/ggml-base.bin，或设置 MULTI_AI_CODE_WHISPER_COMMAND / MULTI_AI_CODE_WHISPER_CMD')
  }

  if (isWhisperCppCommand(input.settings.command)) {
    if (!input.settings.modelPath) {
      throw new Error('本地 Whisper 未配置：whisper-cli 需要模型，请把模型放到 models/whisper/ggml-base.bin，或设置 MULTI_AI_CODE_WHISPER_MODEL')
    }
    return {
      mode: 'execFile',
      command: input.settings.command,
      args: [
        '-m',
        input.settings.modelPath,
        '-f',
        input.audioPath,
        '-l',
        input.settings.language,
        '-otxt',
        '-of',
        input.outputBasePath
      ]
    }
  }

  const args = [
    input.audioPath,
    '--language',
    input.settings.language,
    '--output_format',
    'txt',
    '--output_dir',
    dirname(input.outputBasePath)
  ]
  if (input.settings.modelPath) {
    args.push('--model', input.settings.modelPath)
  }
  return { mode: 'execFile', command: input.settings.command, args }
}

async function defaultFetchArrayBuffer(url: string): Promise<ArrayBuffer> {
  const response = await fetch(url)
  if (!response.ok) {
    throw new Error(`download audio failed: HTTP ${response.status}`)
  }
  return await response.arrayBuffer()
}

function runExecFile(command: string, args: string[], timeoutMs: number) {
  return new Promise<{ stdout: string; stderr: string; exitCode: number }>((resolve) => {
    nodeExecFile(command, args, { timeout: timeoutMs }, (error, stdout, stderr) => {
      const errorMessage = error instanceof Error ? error.message : ''
      resolve({
        stdout: String(stdout ?? ''),
        stderr: String(stderr ?? '') || errorMessage,
        exitCode: typeof (error as { code?: unknown } | null)?.code === 'number'
          ? ((error as { code: number }).code)
          : error
            ? 1
            : 0
      })
    })
  })
}

function runShell(command: string, timeoutMs: number) {
  return new Promise<{ stdout: string; stderr: string; exitCode: number }>((resolve) => {
    nodeExec(command, { timeout: timeoutMs }, (error, stdout, stderr) => {
      resolve({
        stdout: String(stdout ?? ''),
        stderr: String(stderr ?? ''),
        exitCode: typeof (error as { code?: unknown } | null)?.code === 'number'
          ? ((error as { code: number }).code)
          : error
            ? 1
            : 0
      })
    })
  })
}

async function defaultRunCommand(command: LocalWhisperCommand, timeoutMs: number) {
  return command.mode === 'shell'
    ? await runShell(command.command, timeoutMs)
    : await runExecFile(command.command, command.args, timeoutMs)
}

async function defaultReadTextFileIfExists(path: string): Promise<string | null> {
  try {
    return await fs.readFile(path, 'utf8')
  } catch {
    return null
  }
}

function getAudioExtension(url: string): string {
  try {
    const parsed = new URL(url)
    const ext = extname(parsed.pathname)
    return ext || '.amr'
  } catch {
    return '.amr'
  }
}

function cleanTranscript(text: string): string {
  return text
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .join('\n')
    .trim()
}

function getOutputTextPaths(audioPath: string, outputBasePath: string): string[] {
  const pythonWhisperTextPath = join(
    dirname(outputBasePath),
    `${basename(audioPath, extname(audioPath))}.txt`
  )
  return Array.from(new Set([`${outputBasePath}.txt`, pythonWhisperTextPath]))
}

async function prepareAudioPathForWhisper(input: {
  settings: LocalWhisperSettings
  audioPath: string
  tempDir: string
  runCommand: (
    command: LocalWhisperCommand,
    timeoutMs: number
  ) => Promise<{ stdout: string; stderr: string; exitCode: number }>
}): Promise<string> {
  if (!isWhisperCppCommand(input.settings.command)) return input.audioPath

  const extension = extname(input.audioPath).toLowerCase()
  if (WHISPER_CPP_SUPPORTED_EXTENSIONS.has(extension)) return input.audioPath

  if (!input.settings.ffmpegCommand) {
    throw new Error('本地 Whisper 转码未配置：请安装 ffmpeg 或设置 MULTI_AI_CODE_FFMPEG_CMD')
  }

  const wavPath = join(input.tempDir, 'input.wav')
  const result = await input.runCommand(
    {
      mode: 'execFile',
      command: input.settings.ffmpegCommand,
      args: [
        '-y',
        '-i',
        input.audioPath,
        '-ar',
        '16000',
        '-ac',
        '1',
        wavPath
      ]
    },
    input.settings.timeoutMs
  )
  if (result.exitCode !== 0) {
    throw new Error(cleanTranscript(result.stderr || result.stdout) || `ffmpeg exited ${result.exitCode}`)
  }
  return wavPath
}

export async function transcribeRemoteImAudioWithLocalWhisper(
  message: RemoteImIncomingAudioMessage,
  deps: LocalWhisperDeps = {}
): Promise<LocalWhisperResult> {
  const settings = await resolveLocalWhisperSettings(
    readLocalWhisperSettings(deps.env ?? process.env),
    deps
  )
  if (!settings.command && !settings.commandTemplate) {
    return {
      ok: false,
      error: '本地 Whisper 未配置：请把模型放到 models/whisper/ggml-base.bin，或设置 MULTI_AI_CODE_WHISPER_COMMAND / MULTI_AI_CODE_WHISPER_CMD'
    }
  }

  let tempDir: string | null = null
  try {
    tempDir = deps.makeTempDir
      ? await deps.makeTempDir()
      : await fs.mkdtemp(join(tmpdir(), 'multi-ai-im-voice-'))
    const audioPath = join(tempDir, `input${getAudioExtension(message.audioUrl)}`)
    const outputBasePath = join(tempDir, 'transcript')
    const audioBytes = await (deps.fetchArrayBuffer ?? defaultFetchArrayBuffer)(message.audioUrl)
    await (deps.writeFile ?? fs.writeFile)(audioPath, new Uint8Array(audioBytes))

    const runCommand = deps.runCommand ?? defaultRunCommand
    const whisperAudioPath = await prepareAudioPathForWhisper({
      settings,
      audioPath,
      tempDir,
      runCommand
    })
    const command = buildLocalWhisperCommand({
      settings,
      audioPath: whisperAudioPath,
      outputBasePath
    })
    const runResult = await runCommand(command, settings.timeoutMs)
    let fileText: string | null = null
    for (const path of getOutputTextPaths(whisperAudioPath, outputBasePath)) {
      fileText = await (deps.readTextFileIfExists ?? defaultReadTextFileIfExists)(path)
      if (fileText !== null) break
    }
    const transcript = cleanTranscript(fileText ?? runResult.stdout)
    if (runResult.exitCode !== 0) {
      return {
        ok: false,
        error: cleanTranscript(runResult.stderr || runResult.stdout) || `local Whisper exited ${runResult.exitCode}`
      }
    }
    if (!transcript) {
      return { ok: false, error: '本地 Whisper 未输出文字' }
    }
    return { ok: true, text: transcript }
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) }
  } finally {
    if (tempDir) {
      await (deps.cleanupDir ?? ((path) => fs.rm(path, { recursive: true, force: true })))(tempDir)
        .catch(() => undefined)
    }
  }
}
