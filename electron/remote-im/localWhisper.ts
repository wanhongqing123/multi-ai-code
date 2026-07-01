import { execFile as nodeExecFile } from 'child_process'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { basename, dirname, extname, join } from 'path'
import { fileURLToPath } from 'url'
import type { RemoteImIncomingAudioMessage } from './types.js'

export interface LocalWhisperSettings {
  command: string
  modelPath: string
  language: string
  timeoutMs: number
  transcoder: LocalWhisperTranscoder
}

export interface LocalWhisperTranscoder {
  command: string
  kind: 'ffmpeg' | 'afconvert'
}

export interface LocalWhisperCommand {
  mode: 'execFile'
  command: string
  args: string[]
}

export type LocalWhisperResult =
  | { ok: true; text: string }
  | { ok: false; error: string }

export interface LocalWhisperDeps {
  cwd?: string
  moduleUrl?: string
  platform?: NodeJS.Platform
  arch?: NodeJS.Architecture
  resourcesPath?: string
  asrSearchRoots?: string[]
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

export function readLocalWhisperSettings(): LocalWhisperSettings {
  return {
    command: '',
    modelPath: '',
    language: 'zh',
    timeoutMs: 120_000,
    transcoder: { command: '', kind: 'ffmpeg' }
  }
}

const WHISPER_CPP_SUPPORTED_EXTENSIONS = new Set(['.flac', '.mp3', '.ogg', '.wav'])
const ASR_MODEL_NAME = 'ggml-base.bin'

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

function getAsrSearchRoots(input: {
  cwd: string
  moduleUrl: string
  resourcesPath?: string
  explicitRoots?: string[]
}): string[] {
  return uniqueStrings([
    ...(input.explicitRoots ?? []),
    input.resourcesPath ? join(input.resourcesPath, 'asr') : '',
    join(input.cwd, 'resources', 'asr'),
    ...getModuleSearchRoots(input.moduleUrl).map((root) => join(root, 'resources', 'asr'))
  ])
}

function getAsrPlatformKey(platform: NodeJS.Platform, arch: NodeJS.Architecture): string {
  return `${platform}-${arch}`
}

function getAsrExecutableName(platform: NodeJS.Platform, baseName: string): string {
  return platform === 'win32' ? `${baseName}.exe` : baseName
}

function getWhisperCliCandidates(
  roots: string[],
  platform: NodeJS.Platform,
  arch: NodeJS.Architecture
): string[] {
  const executable = getAsrExecutableName(platform, 'whisper-cli')
  const platformKey = getAsrPlatformKey(platform, arch)
  return roots.map((root) => join(root, platformKey, 'bin', executable))
}

function getModelCandidates(roots: string[]): string[] {
  return roots.map((root) => join(root, 'models', ASR_MODEL_NAME))
}

function getTranscoderCandidates(
  roots: string[],
  platform: NodeJS.Platform,
  arch: NodeJS.Architecture
): LocalWhisperTranscoder[] {
  const platformKey = getAsrPlatformKey(platform, arch)
  const bundledFfmpeg = roots.map((root) => ({
    command: join(root, platformKey, 'bin', getAsrExecutableName(platform, 'ffmpeg')),
    kind: 'ffmpeg' as const
  }))
  return platform === 'darwin'
    ? [
        ...bundledFfmpeg,
        { command: '/usr/bin/afconvert', kind: 'afconvert' }
      ]
    : bundledFfmpeg
}

async function firstExistingTranscoder(
  candidates: LocalWhisperTranscoder[],
  deps: Pick<LocalWhisperDeps, 'fileExists'>
): Promise<LocalWhisperTranscoder> {
  const fileExists = deps.fileExists ?? defaultFileExists
  for (const candidate of candidates) {
    if (await fileExists(candidate.command)) return candidate
  }
  return { command: '', kind: 'ffmpeg' }
}

async function resolveLocalWhisperSettings(
  settings: LocalWhisperSettings,
  deps: Pick<
    LocalWhisperDeps,
    'arch' | 'asrSearchRoots' | 'cwd' | 'fileExists' | 'moduleUrl' | 'platform' | 'resourcesPath'
  >
): Promise<LocalWhisperSettings> {
  const resolved = { ...settings }
  const platform = deps.platform ?? process.platform
  const arch = deps.arch ?? process.arch
  const roots = getAsrSearchRoots({
    cwd: deps.cwd ?? process.cwd(),
    moduleUrl: deps.moduleUrl ?? import.meta.url,
    resourcesPath: deps.resourcesPath ?? (process as NodeJS.Process & { resourcesPath?: string }).resourcesPath,
    explicitRoots: deps.asrSearchRoots
  })
  if (!resolved.modelPath) {
    resolved.modelPath = await firstExistingPath(getModelCandidates(roots), deps)
  }
  if (!resolved.command) {
    resolved.command = await firstExistingPath(
      getWhisperCliCandidates(roots, platform, arch),
      deps
    )
  }
  if (!resolved.transcoder.command) {
    resolved.transcoder = await firstExistingTranscoder(
      getTranscoderCandidates(roots, platform, arch),
      deps
    )
  }
  return resolved
}

function isWhisperCppCommand(command: string): boolean {
  const name = basename(command).toLowerCase()
  return name === 'whisper-cli' || name === 'whisper-cli.exe'
}

export function buildLocalWhisperCommand(input: {
  settings: LocalWhisperSettings
  audioPath: string
  outputBasePath: string
}): LocalWhisperCommand {
  if (!input.settings.command) {
    throw new Error('本地语音转文字组件缺失，请重新安装 Multi-AI Code')
  }
  if (!isWhisperCppCommand(input.settings.command)) {
    throw new Error('本地语音转文字组件不可用，请重新安装 Multi-AI Code')
  }
  if (!input.settings.modelPath) {
    throw new Error('本地语音转文字模型缺失，请重新安装 Multi-AI Code')
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

async function defaultRunCommand(command: LocalWhisperCommand, timeoutMs: number) {
  return await runExecFile(command.command, command.args, timeoutMs)
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

function buildTranscodeCommand(input: {
  settings: LocalWhisperSettings
  audioPath: string
  wavPath: string
}): LocalWhisperCommand {
  if (!input.settings.transcoder.command) {
    throw new Error('本地语音转码组件缺失，请重新安装 Multi-AI Code')
  }
  if (input.settings.transcoder.kind === 'afconvert') {
    return {
      mode: 'execFile',
      command: input.settings.transcoder.command,
      args: [
        input.audioPath,
        input.wavPath,
        '-f',
        'WAVE',
        '-d',
        'LEI16@16000',
        '-c',
        '1'
      ]
    }
  }
  return {
    mode: 'execFile',
    command: input.settings.transcoder.command,
    args: [
      '-y',
      '-i',
      input.audioPath,
      '-ar',
      '16000',
      '-ac',
      '1',
      input.wavPath
    ]
  }
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

  const wavPath = join(input.tempDir, 'input.wav')
  const result = await input.runCommand(
    buildTranscodeCommand({ settings: input.settings, audioPath: input.audioPath, wavPath }),
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
    readLocalWhisperSettings(),
    deps
  )
  if (!settings.command || !settings.modelPath) {
    return {
      ok: false,
      error: !settings.command
        ? '本地语音转文字组件缺失，请重新安装 Multi-AI Code'
        : '本地语音转文字模型缺失，请重新安装 Multi-AI Code'
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
